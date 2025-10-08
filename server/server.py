#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32语音助手WebSocket服务器 (简化修复版)

这是一个用于ESP32语音助手项目的WebSocket服务器，实现了与豆包AI语音服务的实时通信。
主要功能包括：
1. 接收来自ESP32的音频数据
2. 将音频数据转发给豆包AI进行语音识别和对话处理
3. 接收豆包AI的语音回应并重采样后发送回ESP32
4. 实现流式音频传输，提供实时对话体验

"""

import asyncio
import websockets
import struct
import json
import gzip
import uuid
import logging
import signal
import sys
from typing import Dict, Any, Optional

# 尝试导入音频处理依赖库
# scipy用于高质量音频重采样，如果未安装则使用简单重采样方法
try:
    import scipy.signal
    import numpy as np
    HAS_SCIPY = True
    print("✅ 已安装scipy，将使用高质量音频重采样")
except ImportError:
    HAS_SCIPY = False
    print("⚠️ 未安装scipy，将使用简单重采样（建议：pip install scipy numpy）")

# 音频采样率配置
ESP32_SAMPLE_RATE = 16000  # ESP32端采样率（Hz）
DOUBAO_SAMPLE_RATE = 24000  # 豆包AI输出采样率（Hz）

# 豆包AI API配置
# 注意：以下密钥为示例，请替换为您自己的密钥
DOUBAO_CONFIG = {
    "base_url": "wss://openspeech.bytedance.com/api/v3/realtime/dialogue",  # 豆包AI WebSocket API地址
    "headers": {
        "X-Api-App-ID": "你的APP ID",           # 应用ID
        "X-Api-Access-Key": "你的 Access Token",  # 访问密钥
        "X-Api-Resource-Id": "volc.speech.dialog",  # 资源ID 固定值
        "X-Api-App-Key": "PlgvMymc7f3tQnJ6",   # 应用密钥 固定值
        "X-Api-Connect-Id": "",                # 连接ID，每次连接时重新生成 固定值
    },
}

# 豆包AI会话配置
SESSION_CONFIG = {
    "asr": {
        "extra": {
            "end_smooth_window_ms": 1500  # ASR结束平滑窗口时间（毫秒）
        }
    },
    "tts": {
        "speaker": "zh_male_yunzhou_jupiter_bigtts",  # TTS发音人
        "audio_config": {
            "channel": 1,           # 音频通道数
            "format": "pcm",        # 音频格式
            "sample_rate": 24000    # 音频采样率（Hz）
        }
    },
    "dialog": {
        "bot_name": "小智",  # AI助手名称
        "system_role": "你是一个友好、耐心、乐于助人的AI助手，名叫小智。请保持回答简洁，通常一到两句话。",  # AI角色设定
        "speaking_style": "你的说话风格简洁明了，语速适中，语调自然。",  # 说话风格
        "location": {"city": "北京"},  # 位置信息
        "extra": {
            "strict_audit": False,   # 是否严格审核
            "recv_timeout": 10,      # 接收超时时间（秒）
            "input_mod": "audio"     # 输入模式
        }
    },
}

# 设置日志配置
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# 全局变量用于优雅关闭
server = None
running = True

def resample_audio_24k_to_16k(audio_data: bytes) -> bytes:
    """
    将24kHz音频重采样为16kHz并转换为int16格式
    
    这是解决音频杂音问题的核心功能。豆包AI输出24kHz音频，而ESP32需要16kHz音频，
    因此需要进行重采样处理。
    
    Args:
        audio_data (bytes): 原始24kHz音频数据
        
    Returns:
        bytes: 重采样后的16kHz音频数据
    """
    # 如果没有音频数据，直接返回
    if not audio_data:
        return audio_data
    
    try:
        # 如果安装了scipy库，使用高质量重采样方法
        if HAS_SCIPY:
            # 将音频数据转换为numpy数组（float32格式）
            audio_samples = np.frombuffer(audio_data, dtype=np.float32)
            if len(audio_samples) == 0:
                return audio_data
            
            # 计算目标采样点数
            target_length = int(len(audio_samples) * ESP32_SAMPLE_RATE / DOUBAO_SAMPLE_RATE)
            
            # 使用scipy进行重采样
            resampled_samples = scipy.signal.resample(audio_samples, target_length)
            
            # 将float32转换为int16格式
            # 将浮点数范围[-1.0, 1.0]缩放到整数范围[-32768, 32767]
            int16_samples = (resampled_samples * 32767).astype(np.int16)
            return int16_samples.tobytes()
        else:
            # 如果没有scipy库，使用简单重采样方法
            # 每3个24kHz样本取2个16kHz样本
            samples = struct.unpack(f'<{len(audio_data)//4}f', audio_data)
            resampled = []
            for i in range(0, len(samples), 3):
                if i < len(samples):
                    resampled.append(samples[i])
                if i + 1 < len(samples):
                    resampled.append(samples[i + 1])
            
            # 转换为int16格式
            int16_samples = [int(max(-32768, min(32767, sample * 32767))) for sample in resampled]
            return struct.pack(f'<{len(int16_samples)}h', *int16_samples)
    except Exception as e:
        logger.error(f"音频重采样失败: {e}")
        return audio_data

def create_protocol_header(message_type=0b0001, has_event=True, use_json=True, use_gzip=True):
    """
    创建豆包AI协议头
    
    Args:
        message_type (int): 消息类型
        has_event (bool): 是否包含事件
        use_json (bool): 是否使用JSON格式
        use_gzip (bool): 是否使用gzip压缩
        
    Returns:
        bytearray: 协议头数据
    """
    header = bytearray()
    header_size = 1
    version = 0b0001
    header.append((version << 4) | header_size)
    flags = 0b0100 if has_event else 0b0000
    header.append((message_type << 4) | flags)
    serial = 0b0001 if use_json else 0b0000
    compress = 0b0001 if use_gzip else 0b0000
    header.append((serial << 4) | compress)
    header.append(0x00)
    return header

def parse_doubao_response(data: bytes) -> Dict[str, Any]:
    """
    解析豆包AI服务器响应
    
    Args:
        data (bytes): 服务器响应数据
        
    Returns:
        Dict[str, Any]: 解析后的响应数据
    """
    # 如果数据为空或长度不足，返回空字典
    if isinstance(data, str) or len(data) < 4:
        return {}
    
    try:
        # 解析协议头
        header_size = (data[0] & 0x0f) * 4
        message_type = data[1] >> 4
        message_flags = data[1] & 0x0f
        has_event = bool(message_flags & 0b0100)
        compression = data[2] & 0x0f
        use_gzip = bool(compression)
        use_json = bool(data[2] >> 4)
        
        # 提取有效载荷数据
        payload = data[header_size:]
        result = {"message_type": "unknown"}
        offset = 0
        
        # 解析事件ID
        if has_event and len(payload) >= offset + 4:
            result["event"] = int.from_bytes(payload[offset:offset+4], "big")
            offset += 4
        
        # 解析会话ID
        if len(payload) >= offset + 4:
            session_id_len = int.from_bytes(payload[offset:offset+4], "big")
            offset += 4
            if len(payload) >= offset + session_id_len:
                session_id = payload[offset:offset+session_id_len]
                result["session_id"] = session_id.decode('utf-8', errors='ignore')
                offset += session_id_len
        
        # 解析消息数据
        if len(payload) >= offset + 4:
            msg_len = int.from_bytes(payload[offset:offset+4], "big")
            offset += 4
            if len(payload) >= offset + msg_len:
                msg_data = payload[offset:offset+msg_len]
                
                # 解压缩数据
                if use_gzip and msg_data:
                    try:
                        msg_data = gzip.decompress(msg_data)
                    except:
                        pass  # 如果解压缩失败，使用原始数据
                
                # 根据消息类型处理数据
                if message_type == 0b1011:  # SERVER_ACK（音频数据）
                    result["message_type"] = "audio"
                    if use_json and msg_data:
                        try:
                            result["payload"] = json.loads(msg_data.decode('utf-8'))
                        except:
                            # 不是JSON格式，直接作为音频数据处理
                            logger.info(f"🎵 接收到豆包音频数据: {len(msg_data)} 字节，正在重采样...")
                            resampled_audio = resample_audio_24k_to_16k(msg_data)
                            result["audio_data"] = resampled_audio
                            logger.info(f"✅ 重采样完成: {len(resampled_audio)} 字节")
                    else:
                        # 直接是音频数据
                        logger.info(f"🎵 接收到豆包音频数据: {len(msg_data)} 字节，正在重采样...")
                        resampled_audio = resample_audio_24k_to_16k(msg_data)
                        result["audio_data"] = resampled_audio
                        logger.info(f"✅ 重采样完成: {len(resampled_audio)} 字节")
                elif message_type == 0b1001:  # SERVER_FULL_RESPONSE（完整响应）
                    result["message_type"] = "response"
                    if use_json and msg_data:
                        try:
                            result["payload"] = json.loads(msg_data.decode('utf-8'))
                        except:
                            result["payload"] = msg_data
                    else:
                        result["payload"] = msg_data
        
        return result
    except Exception as e:
        logger.error(f"解析豆包响应失败: {e}")
        return {}

async def safe_send(websocket, data):
    """
    安全地向WebSocket发送数据
    
    Args:
        websocket: WebSocket连接对象
        data: 要发送的数据
    """
    if websocket is None or websocket.closed:
        return False
        
    try:
        await websocket.send(data)
        return True
    except Exception as e:
        logger.debug(f"发送数据失败（连接可能已关闭）: {e}")
        return False

async def handle_esp32_client(websocket, path):
    """
    处理ESP32客户端连接
    
    Args:
        websocket: WebSocket连接对象
        path: 请求路径
    """
    client_address = websocket.remote_address
    logger.info(f"🔗 ESP32客户端连接: {client_address}")
    
    # 初始化变量
    doubao_ws = None
    audio_stream_buffer = b''  # 音频流缓冲区
    tasks = []  # 存储任务引用以便正确清理
    
    try:
        # 1. 连接豆包AI服务器
        session_id = str(uuid.uuid4())
        DOUBAO_CONFIG["headers"]["X-Api-Connect-Id"] = str(uuid.uuid4())
        
        logger.info("🚀 正在连接豆包服务器...")
        doubao_ws = await websockets.connect(
            DOUBAO_CONFIG['base_url'],
            extra_headers=DOUBAO_CONFIG['headers'],
            ping_interval=None,
        )
        logger.info("✅ 豆包服务器连接成功！")
        
        # 2. 初始化豆包AI会话
        # 2.1 发送StartConnection消息
        header = create_protocol_header()
        message = bytearray(header)
        message.extend((1).to_bytes(4, 'big'))
        payload = gzip.compress(b"{}")
        message.extend(len(payload).to_bytes(4, 'big'))
        message.extend(payload)
        
        if not doubao_ws.closed:
            await doubao_ws.send(message)
            await doubao_ws.recv()  # 接收确认响应
        
        # 2.2 发送StartSession消息
        header = create_protocol_header()
        message = bytearray(header)
        message.extend((100).to_bytes(4, 'big'))
        session_bytes = session_id.encode('utf-8')
        message.extend(len(session_bytes).to_bytes(4, 'big'))
        message.extend(session_bytes)
        payload = gzip.compress(json.dumps(SESSION_CONFIG).encode('utf-8'))
        message.extend(len(payload).to_bytes(4, 'big'))
        message.extend(payload)
        
        if not doubao_ws.closed:
            await doubao_ws.send(message)
            await doubao_ws.recv()  # 接收确认响应
        logger.info("✅ 豆包会话初始化完成")
        
        # 向ESP32发送就绪消息
        await safe_send(websocket, json.dumps({
            "type": "ready",
            "message": "🎤 服务器已就绪，可以开始语音对话"
        }))
        
        # 3. 创建双向数据转发任务
        async def forward_esp32_to_doubao():
            """
            转发ESP32音频数据到豆包AI
            """
            try:
                async for audio_chunk in websocket:
                    if isinstance(audio_chunk, bytes) and doubao_ws and not doubao_ws.closed:
                        # 构造并发送音频数据到豆包AI
                        header = create_protocol_header(message_type=0b0010, use_json=False)
                        message = bytearray(header)
                        message.extend((200).to_bytes(4, 'big'))
                        session_bytes = session_id.encode('utf-8')
                        message.extend(len(session_bytes).to_bytes(4, 'big'))
                        message.extend(session_bytes)
                        compressed_audio = gzip.compress(audio_chunk)
                        message.extend(len(compressed_audio).to_bytes(4, 'big'))
                        message.extend(compressed_audio)
                        
                        try:
                            await doubao_ws.send(message)
                            logger.info(f"🎵 转发音频到豆包: {len(audio_chunk)} 字节")
                        except Exception as e:
                            logger.warning(f"转发音频到豆包失败: {e}")
                            break
            except Exception as e:
                logger.debug(f"ESP32音频转发任务结束: {e}")
        
        async def forward_doubao_to_esp32():
            """
            转发豆包AI响应到ESP32（流式版本）
            """
            nonlocal audio_stream_buffer
            
            try:
                while True:
                    response_data = await doubao_ws.recv()
                    response = parse_doubao_response(response_data)
                    if not response:
                        continue
                    
                    # 处理音频数据
                    if "audio_data" in response:
                        audio_data = response["audio_data"]
                        if len(audio_data) > 0:
                            # 将音频数据添加到流缓冲区
                            audio_stream_buffer += audio_data
                            logger.debug(f"🔊 添加音频数据到缓冲区: {len(audio_data)} 字节，缓冲区总大小: {len(audio_stream_buffer)} 字节")
                            
                            # 当缓冲区达到一定大小时，发送给ESP32
                            chunk_size = 1600  # 50ms的音频数据 (16000Hz * 0.05s * 2bytes)
                            
                            while len(audio_stream_buffer) >= chunk_size:
                                # 取出一个块发送
                                chunk = audio_stream_buffer[:chunk_size]
                                audio_stream_buffer = audio_stream_buffer[chunk_size:]
                                
                                # 检查音频数据有效性（确保是整数采样）
                                if len(chunk) % 2 != 0:
                                    logger.debug(f"⚠️ 过滤非整数采样数据: {len(chunk)} 字节")
                                    continue
                                
                                if not await safe_send(websocket, chunk):
                                    logger.warning("ESP32连接已关闭，无法发送音频")
                                    return
                                
                                logger.info(f"🔊 发送音频块到ESP32: {len(chunk)} 字节")
                                
                                # 稍微延迟，保持流式播放的均匀性
                                await asyncio.sleep(0.01)  # 10ms延迟
                    
                    # 处理其他响应数据
                    elif "payload" in response:
                        event = response.get("event")
                        payload = response["payload"]
                        
                        # 处理ASR结果（语音识别结果）
                        if event == 451 and isinstance(payload, dict) and "results" in payload:
                            if payload["results"] and not payload["results"][0].get("is_interim", True):
                                text = payload["results"][0].get("text", "")
                                logger.info(f"👤 用户说: {text}")
                                
                        # 处理TTS结束事件
                        elif event == 559:
                            # 等待一段时间确保所有音频数据发送完成
                            await asyncio.sleep(0.2)
                            
                            # TTS结束，发送剩余的音频数据
                            if len(audio_stream_buffer) > 0:
                                logger.info(f"🎵 TTS结束，发送剩余音频: {len(audio_stream_buffer)} 字节")
                                if len(audio_stream_buffer) % 2 == 0:  # 确保整数采样
                                    if not await safe_send(websocket, audio_stream_buffer):
                                        logger.warning("ESP32连接已关闭，无法发送剩余音频")
                                
                                audio_stream_buffer = b''  # 清空缓冲区
                            
                            # 等待确保剩余音频数据发送完成
                            await asyncio.sleep(0.1)
                            
                            # 再次发送一段静音数据确保缓冲区清空
                            silence_data = bytes([0] * 1024)  # 1KB静音数据
                            if not await safe_send(websocket, silence_data):
                                logger.warning("ESP32连接已关闭，无法发送静音数据")
                            
                            # 等待确保静音数据发送完成
                            await asyncio.sleep(0.05)
                            
                            # 发送明确的停止播放信号
                            if not await safe_send(websocket, json.dumps({
                                "type": "tts_end",
                                "message": "TTS结束，停止流式播放"
                            })):
                                logger.warning("ESP32连接已关闭，无法发送停止信号")
                            
                            logger.info("🤖 AI回复结束，已发送停止信号")
                            
            except Exception as e:
                logger.debug(f"豆包响应转发任务结束: {e}")
        
        # 创建并运行双向转发任务
        task1 = asyncio.create_task(forward_esp32_to_doubao())
        task2 = asyncio.create_task(forward_doubao_to_esp32())
        tasks = [task1, task2]
        
        # 等待任一任务完成或出现异常
        done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        
        # 取消未完成的任务
        for task in pending:
            task.cancel()
            
    except Exception as e:
        logger.error(f"处理ESP32客户端失败: {e}")
        
    finally:
        # 清理资源
        logger.info("🧹 清理连接资源...")
        
        # 取消所有运行中的任务
        for task in tasks:
            if not task.done():
                task.cancel()
        
        # 关闭豆包WebSocket连接
        if doubao_ws:
            try:
                # 等待一小段时间确保所有数据发送完成
                await asyncio.sleep(0.1)
                if not doubao_ws.closed:
                    await doubao_ws.close()
                logger.info("✅ 豆包连接已关闭")
            except Exception as e:
                logger.debug(f"关闭豆包连接时出错（可能是正常关闭）: {e}")
        
        logger.info(f"✅ 客户端 {client_address} 处理完成")

def signal_handler(signum, frame):
    """
    信号处理函数，用于优雅关闭服务器
    """
    global running, server
    logger.info("👋 收到停止信号")
    running = False
    
    # 关闭服务器
    if server:
        server.close()
    
    # 退出程序
    sys.exit(0)

async def main():
    """
    主函数
    启动WebSocket服务器并等待连接
    """
    global server, running
    
    host = "0.0.0.0"  # 监听所有网络接口
    port = 8888       # 监听端口
    
    logger.info("=" * 60)
    logger.info("🎯 ESP32语音助手服务器 (杂音修复版)")
    logger.info("=" * 60)
    logger.info(f"📡 关键修复: 豆包24kHz -> ESP32 16kHz音频重采样")
    logger.info(f"🚀 服务器启动: ws://{host}:{port}")
    logger.info("=" * 60)
    logger.info("等待ESP32连接...")
    
    # 注册信号处理器
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    try:
        server = await websockets.serve(handle_esp32_client, host, port)
        logger.info("✅ WebSocket服务器启动成功")
        
        # 保持服务器运行
        while running:
            await asyncio.sleep(1)
            
    except Exception as e:
        logger.error(f"服务器运行出错: {e}")
    finally:
        logger.info("🛑 正在关闭服务器...")
        
        # 关闭服务器
        if server:
            try:
                server.close()
                await server.wait_closed()
                logger.info("✅ WebSocket服务器已关闭")
            except Exception as e:
                logger.debug(f"关闭服务器时出错（可能是正常关闭）: {e}")
        
        logger.info("🛑 服务器已停止")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("👋 程序被用户中断")
    except Exception as e:
        logger.error(f"程序运行出错: {e}")