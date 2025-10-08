/**
 * @file audio_manager.cc
 * @brief 🎧 音频管理器实现文件 (已修改为发送原始PCM数据)
 * * 这里实现了audio_manager.h中声明的所有功能。
 * 主要包括录音缓冲区管理、音频播放控制和流式播放。
 */

extern "C" {
#include <string.h>
#include "esp_log.h"
#include "bsp_board.h"
}

#include "audio_manager.h"

const char* AudioManager::TAG = "AudioManager";

AudioManager::AudioManager(uint32_t sample_rate, uint32_t recording_duration_sec, uint32_t response_duration_sec)
    : sample_rate(sample_rate)
    , recording_duration_sec(recording_duration_sec)
    , response_duration_sec(response_duration_sec)
    , recording_buffer(nullptr)
    , recording_buffer_size(0)
    , recording_length(0)
    , is_recording(false)
    , response_buffer(nullptr)
    , response_buffer_size(0)
    , response_length(0)
    , response_played(false)
    , is_streaming(false)
    , streaming_buffer(nullptr)
    , streaming_buffer_size(STREAMING_BUFFER_SIZE)
    , streaming_write_pos(0)
    , streaming_read_pos(0)
{
    recording_buffer_size = sample_rate * recording_duration_sec * sizeof(int16_t);
    response_buffer_size = sample_rate * response_duration_sec * sizeof(int16_t);

    ESP_LOGI(TAG, "初始化音频管理器...");
    recording_buffer = (int16_t*)malloc(recording_buffer_size);
    if (recording_buffer) {
        ESP_LOGI(TAG, "✓ 录音缓冲区分配成功，大小: %zu 字节 (%d 秒)", recording_buffer_size, recording_duration_sec);
    } else {
        ESP_LOGE(TAG, "❌ 录音缓冲区分配失败");
    }

    response_buffer = (int16_t*)malloc(response_buffer_size);
    if (response_buffer) {
        ESP_LOGI(TAG, "✓ 响应缓冲区分配成功，大小: %zu 字节 (%d 秒)", response_buffer_size, response_duration_sec);
    } else {
        ESP_LOGE(TAG, "❌ 响应缓冲区分配失败");
    }

    streaming_buffer = (uint8_t*)malloc(streaming_buffer_size);
    if (streaming_buffer) {
        ESP_LOGI(TAG, "✓ 流式播放缓冲区分配成功，大小: %zu 字节", streaming_buffer_size);
    } else {
        ESP_LOGE(TAG, "❌ 流式播放缓冲区分配失败");
    }
}

AudioManager::~AudioManager() {
    free(recording_buffer);
    free(response_buffer);
    free(streaming_buffer);
}

void AudioManager::start_recording() {
    if (!is_recording) {
        ESP_LOGI(TAG, "开始录音...");
        recording_length = 0;
        is_recording = true;
        // 注释了未定义的函数调用
        // bsp_record_start();
    }
}

void AudioManager::stop_recording() {
    if (is_recording) {
        is_recording = false;
        // 注释了未定义的函数调用
        // bsp_record_stop();
        ESP_LOGI(TAG, "停止录音，当前长度: %zu 样本 (%.2f 秒)", recording_length, (float)recording_length / sample_rate);
    }
}

esp_err_t AudioManager::play_audio(const uint8_t* data, size_t len) {
    ESP_LOGI(TAG, "播放音频...");
    esp_err_t ret = bsp_play_audio(data, len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ 音频播放成功");
    } else {
        ESP_LOGE(TAG, "❌ 音频播放失败");
    }
    return ret;
}

void AudioManager::audio_record_task(void *arg) {
    AudioManager *self = (AudioManager *)arg;
    size_t pcm_data_size = self->sample_rate * 20 / 1000 * 2;
    int16_t *pcm_data = (int16_t *)malloc(pcm_data_size);

    while (true) {
        if (!self->is_recording) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bsp_get_feed_data(false, pcm_data, pcm_data_size);

        if (pcm_data_size > 0) {
            if (self->recording_length + pcm_data_size / sizeof(int16_t) <= self->recording_buffer_size / sizeof(int16_t)) {
                memcpy(self->recording_buffer + self->recording_length, pcm_data, pcm_data_size);
                self->recording_length += pcm_data_size / sizeof(int16_t);
            } else {
                ESP_LOGW(TAG, "录音缓冲区已满（超过%d秒上限）", self->recording_duration_sec);
            }

            // 为了发送到队列，我们需要为数据单独分配内存
            uint8_t* data_to_send = (uint8_t*)malloc(pcm_data_size);
            if (data_to_send) {
                memcpy(data_to_send, pcm_data, pcm_data_size);
                AudioQueueItem item = { data_to_send, pcm_data_size };
                // 添加检查，确保队列未满
                if (uxQueueSpacesAvailable(s_audio_send_queue) > 0) {
                    xQueueSend(s_audio_send_queue, &item, 0);
                } else {
                    ESP_LOGW(TAG, "音频发送队列已满，丢弃数据");
                    free(data_to_send);
                }
            }
        }
        // 添加延迟以控制录音速率
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    free(pcm_data);
    vTaskDelete(NULL);
}


void AudioManager::start_streaming_playback() {
    // 🔑 关键修复：先停止旧的流式播放，再启动新的
    stop_streaming_playback();
    
    // 🎯 采用千问项目的同步播放方法，不创建异步任务
    ESP_LOGI(TAG, "🎵 启动同步流式音频播放模式");
    is_streaming = true;
    
    // 清空缓冲区
    if (streaming_buffer) {
        memset(streaming_buffer, 0, streaming_buffer_size);
    }
    streaming_write_pos = 0;
    streaming_read_pos = 0;
    
    ESP_LOGI(TAG, "✅ 流式播放已就绪，采用即时播放模式");
}

void AudioManager::stop_streaming_playback() {
    if (is_streaming) {
        ESP_LOGI(TAG, "📍 停止流式播放，等待任务退出...");
        is_streaming = false;
        
        // 🔇 立即停止I2S以防止重复播放
        bsp_audio_stop();
        
        // 🔇 关键修复（参考千问项目）：处理最后的尾巴数据
        size_t remaining_data;
        if (streaming_write_pos >= streaming_read_pos) {
            remaining_data = streaming_write_pos - streaming_read_pos;
        } else {
            remaining_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
        }
        
        if (remaining_data > 0 && remaining_data <= 16384) { // 只处理小于16KB的尾巴数据
            ESP_LOGI(TAG, "🎹 播放尾巴数据: %zu 字节", remaining_data);
            // 分配临时缓冲区
            uint8_t* remaining_buffer = (uint8_t*)malloc(remaining_data);
            if (remaining_buffer) {
                // 读取所有尾巴数据
                if (streaming_write_pos >= streaming_read_pos) {
                    memcpy(remaining_buffer, streaming_buffer + streaming_read_pos, remaining_data);
                } else {
                    size_t bytes_to_end = streaming_buffer_size - streaming_read_pos;
                    memcpy(remaining_buffer, streaming_buffer + streaming_read_pos, bytes_to_end);
                    memcpy(remaining_buffer + bytes_to_end, streaming_buffer, streaming_write_pos);
                }
                
                // 🔇 关键：使用bsp_play_audio（普通版本，会自动停止I2S）
                esp_err_t ret = bsp_play_audio(remaining_buffer, remaining_data);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "✅ 尾巴音频播放完成并自动停止I2S，解决重复问题");
                } else {
                    ESP_LOGW(TAG, "⚠️ 尾巴音频播放失败: %s", esp_err_to_name(ret));
                    // 如果播放失败，手动停止I2S
                    bsp_audio_stop();
                }
                free(remaining_buffer);
            }
        } else if (remaining_data > 16384) {
            ESP_LOGW(TAG, "跳过过大的尾巴数据: %zu 字节", remaining_data);
            // 手动停止I2S
            bsp_audio_stop();
        } else {
            // 没有尾巴数据，直接停止I2S
            bsp_audio_stop();
        }
        
        // 等待流式播放任务退出
        vTaskDelay(pdMS_TO_TICKS(200));
        
        // 🧹 清空软件缓冲区
        if (streaming_buffer) {
            memset(streaming_buffer, 0, streaming_buffer_size);
            streaming_write_pos = 0;
            streaming_read_pos = 0;
            ESP_LOGI(TAG, "✅ 流式播放缓冲区已清空");
        }
        
        ESP_LOGI(TAG, "✅ 流式播放已完全停止，重复音频已消除");
    }
}

void AudioManager::feed_streaming_audio(const uint8_t* data, size_t len) {
    if (!is_streaming) {
        ESP_LOGW(TAG, "流式播放未启动，丢弃音频数据: %zu 字节", len);
        return;
    }

    // 🔍 加强无效数据过滤：太小或奇数长度的数据包
    if (len < 128) {  // 提高到128字节，过滤更多小数据包
        ESP_LOGD(TAG, "过滤小数据包: %zu 字节（可能是控制消息）", len);
        return;
    }
    
    // 验证数据长度是否为偶数（因16位PCM）
    if (len % 2 != 0) {
        ESP_LOGW(TAG, "跳过奇数长度的数据包: %zu 字节（不是有效的PCM数据）", len);
        return;
    }
    
    // 🎯 新增：检查数据内容的有效性，过滤全零或全相同的数据
    if (len >= sizeof(int16_t) * 4) { // 至少检查4个样本
        int16_t* samples = (int16_t*)data;
        size_t sample_count = len / sizeof(int16_t);
        bool has_variation = false;
        
        // 检查是否有变化（相邻样本差值大于阈值）
        for (size_t i = 1; i < sample_count && !has_variation; i++) {
            if (abs(samples[i] - samples[i-1]) > 30) {
                has_variation = true;
            }
        }
        
        if (!has_variation) {
            ESP_LOGD(TAG, "过滤静音/无效数据包: %zu 字节（无音频变化）", len);
            return;
        }
    }

    ESP_LOGD(TAG, "接收到流式音频数据: %zu 字节", len);

    // 🔇 关键修复（采用千问项目方法）：直接同步播放，不使用异步任务
    // 🔍 检查是否有足够的数据可以播放（累积25ms数据）
    const size_t chunk_size = 25 * (sample_rate / 1000) * sizeof(int16_t); // 25ms数据
    
    // 直接播放音频数据块，不缓存
    if (len >= chunk_size) {
        // 大块数据，分块播放
        size_t offset = 0;
        while (offset + chunk_size <= len && is_streaming) {
            esp_err_t ret = bsp_play_audio_stream(data + offset, chunk_size);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "流式音频播放失败: %s", esp_err_to_name(ret));
                break;
            }
            offset += chunk_size;
            ESP_LOGD(TAG, "播放音频块: %zu 字节", chunk_size);
        }
        
        // 处理剩余的小块数据
        size_t remaining = len - offset;
        if (remaining > 0 && is_streaming) {
            // 将小块数据存入缓冲区等待下次合并
            if (streaming_buffer && remaining < streaming_buffer_size) {
                memcpy(streaming_buffer, data + offset, remaining);
                streaming_write_pos = remaining;
                streaming_read_pos = 0;
                ESP_LOGD(TAG, "缓存小块数据: %zu 字节", remaining);
            }
        }
    } else {
        // 小块数据，与缓冲区中的数据合并
        if (streaming_buffer) {
            size_t buffered_data = streaming_write_pos - streaming_read_pos;
            if (buffered_data + len >= chunk_size) {
                // 合并后足够播放
                uint8_t* combined_buffer = (uint8_t*)malloc(buffered_data + len);
                if (combined_buffer) {
                    memcpy(combined_buffer, streaming_buffer + streaming_read_pos, buffered_data);
                    memcpy(combined_buffer + buffered_data, data, len);
                    
                    // 播放合并后的数据
                    esp_err_t ret = bsp_play_audio_stream(combined_buffer, buffered_data + len);
                    if (ret == ESP_OK) {
                        ESP_LOGD(TAG, "播放合并音频: %zu 字节", buffered_data + len);
                        // 清空缓冲区
                        streaming_write_pos = 0;
                        streaming_read_pos = 0;
                    } else {
                        ESP_LOGW(TAG, "合并音频播放失败: %s", esp_err_to_name(ret));
                    }
                    free(combined_buffer);
                }
            } else {
                // 数据仍不足，继续缓存
                if (streaming_write_pos + len < streaming_buffer_size) {
                    memcpy(streaming_buffer + streaming_write_pos, data, len);
                    streaming_write_pos += len;
                    ESP_LOGD(TAG, "继续缓存数据: %zu 字节，总计: %zu 字节", 
                            len, streaming_write_pos - streaming_read_pos);
                }
            }
        }
    }
}

void AudioManager::streaming_playback_task(void* arg) {
    AudioManager* self = (AudioManager*)arg;
    // 🎯 减小播放块大小到25ms，提高音频流的实时性和稳定性
    const size_t play_chunk_size = 25 * (self->sample_rate / 1000) * sizeof(int16_t); // 25ms 的数据
    uint8_t* play_buffer = (uint8_t*)malloc(play_chunk_size);
    
    if (!play_buffer) {
        ESP_LOGE(TAG, "❌ 无法分配播放缓冲区");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "🎵 开始流式网络音频播放，块大小: %zu 字节 (25ms)", play_chunk_size);
    
    // 🔧 播放开始前先发送一小段静音，确保I2S通道稳定
    const size_t init_silence_size = 320; // 10ms的静音数据 
    uint8_t* silence_buffer = (uint8_t*)calloc(init_silence_size, 1);
    if (silence_buffer) {
        bsp_play_audio_stream(silence_buffer, init_silence_size);
        free(silence_buffer);
        ESP_LOGD(TAG, "✅ 已发送初始化静音数据");
    }

    while (self->is_streaming) {
        // 🔍 首先检查是否仍在流式播放状态
        if (!self->is_streaming) {
            ESP_LOGI(TAG, "📍 检测到停止信号，立即退出播放循环");
            break;
        }
        
        size_t data_available;
        if (self->streaming_write_pos >= self->streaming_read_pos) {
            data_available = self->streaming_write_pos - self->streaming_read_pos;
        } else {
            data_available = self->streaming_buffer_size - self->streaming_read_pos + self->streaming_write_pos;
        }

        if (data_available >= play_chunk_size) {
            // 🔧 确保读指针对齐到16位边界，防止数据错位
            if (self->streaming_read_pos % 2 != 0) {
                ESP_LOGW(TAG, "修复读指针对齐: %zu -> %zu", 
                        self->streaming_read_pos, (self->streaming_read_pos + 1) % self->streaming_buffer_size);
                self->streaming_read_pos = (self->streaming_read_pos + 1) % self->streaming_buffer_size;
                // 重新计算可用数据
                if (self->streaming_write_pos >= self->streaming_read_pos) {
                    data_available = self->streaming_write_pos - self->streaming_read_pos;
                } else {
                    data_available = self->streaming_buffer_size - self->streaming_read_pos + self->streaming_write_pos;
                }
                if (data_available < play_chunk_size) {
                    continue; // 对齐后数据不足，继续等待
                }
            }
            
            // 从环形缓冲区读取数据
            if (self->streaming_read_pos + play_chunk_size <= self->streaming_buffer_size) {
                memcpy(play_buffer, self->streaming_buffer + self->streaming_read_pos, play_chunk_size);
                self->streaming_read_pos = (self->streaming_read_pos + play_chunk_size) % self->streaming_buffer_size;
            } else {
                size_t first_chunk_size = self->streaming_buffer_size - self->streaming_read_pos;
                memcpy(play_buffer, self->streaming_buffer + self->streaming_read_pos, first_chunk_size);
                size_t second_chunk_size = play_chunk_size - first_chunk_size;
                memcpy(play_buffer + first_chunk_size, self->streaming_buffer, second_chunk_size);
                self->streaming_read_pos = second_chunk_size;
            }
            
            // 🎯 验证音频数据的有效性，过滤异常数据
            bool is_valid_audio = false;
            int16_t* samples = (int16_t*)play_buffer;
            size_t sample_count = play_chunk_size / sizeof(int16_t);
            
            // 检查是否包含有效的音频信号（非全零或全相同）
            for (size_t i = 1; i < sample_count && !is_valid_audio; i++) {
                if (abs(samples[i] - samples[i-1]) > 50) { // 相邻样本差值大于50认为是有效信号
                    is_valid_audio = true;
                }
            }
            
            // 🔧 关键修复：确保音频数据16位对齐，防止数据错位导致杂音
            if (is_valid_audio && self->is_streaming) { // 播放前再次检查状态
                // 播放音频数据
                esp_err_t ret = bsp_play_audio_stream(play_buffer, play_chunk_size);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "流式音频播放失败: %s", esp_err_to_name(ret));
                    // 播放失败时重置I2S状态，防止累积错误
                    bsp_audio_stop();
                    vTaskDelay(pdMS_TO_TICKS(50));
                } else {
                    ESP_LOGD(TAG, "播放网络音频块: %zu 字节 (有效信号)", play_chunk_size);
                }
            } else if (!self->is_streaming) {
                ESP_LOGI(TAG, "📍 检测到停止信号，跳过播放");
                break;
            } else {
                // 跳过无效音频数据，避免播放杂音
                ESP_LOGD(TAG, "跳过无效音频块: %zu 字节 (静音/噪音)", play_chunk_size);
            }
        } else if (data_available > 0) {
            // 有数据但不足一个完整块，等待更多数据
            ESP_LOGD(TAG, "等待更多网络音频数据，当前: %zu 字节，需要: %zu 字节", 
                    data_available, play_chunk_size);
            vTaskDelay(pdMS_TO_TICKS(3)); // 更短的等待时间提高响应性
        } else {
            // 没有数据，等待稍久
            vTaskDelay(pdMS_TO_TICKS(8));
        }
    }
    
    ESP_LOGI(TAG, "结束流式网络音频播放");
    
    // 🔇 关键修复：播放结束时立即停止I2S，防止重复最后一个字
    bsp_audio_stop();
    ESP_LOGI(TAG, "✅ 已停止I2S输出，防止重复播放最后音频");
    
    // 播放剩余的音频数据（如果有）
    size_t remaining_data;
    if (self->streaming_write_pos >= self->streaming_read_pos) {
        remaining_data = self->streaming_write_pos - self->streaming_read_pos;
    } else {
        remaining_data = self->streaming_buffer_size - self->streaming_read_pos + self->streaming_write_pos;
    }
    
    if (remaining_data > 0 && remaining_data <= 16384) { // 只播放小于16KB的剩余数据
        ESP_LOGI(TAG, "播放剩余的网络音频数据: %zu 字节", remaining_data);
        uint8_t* remaining_buffer = (uint8_t*)malloc(remaining_data);
        if (remaining_buffer) {
            if (self->streaming_write_pos >= self->streaming_read_pos) {
                memcpy(remaining_buffer, self->streaming_buffer + self->streaming_read_pos, remaining_data);
            } else {
                size_t bytes_to_end = self->streaming_buffer_size - self->streaming_read_pos;
                memcpy(remaining_buffer, self->streaming_buffer + self->streaming_read_pos, bytes_to_end);
                memcpy(remaining_buffer + bytes_to_end, self->streaming_buffer, self->streaming_write_pos);
            }
            
            esp_err_t ret = bsp_play_audio(remaining_buffer, remaining_data);
            if (ret == ESP_OK) {
                // 🔇 播放完剩余数据后立即停止I2S
                bsp_audio_stop();
                ESP_LOGI(TAG, "✅ 剩余音频播放完成并已停止I2S");
            } else {
                ESP_LOGW(TAG, "播放剩余音频失败: %s", esp_err_to_name(ret));
            }
            free(remaining_buffer);
        }
    } else if (remaining_data > 16384) {
        ESP_LOGW(TAG, "跳过过大的剩余数据: %zu 字节", remaining_data);
    }
    
    // 🧹 清理任务结束前的状态
    ESP_LOGI(TAG, "🧹 清理流式播放任务状态");
    memset(self->streaming_buffer, 0, self->streaming_buffer_size);
    self->streaming_write_pos = 0;
    self->streaming_read_pos = 0;
    
    // 🎯 任务退出时停止I2S硬件，确保没有残留音频
    bsp_audio_stop();
    ESP_LOGI(TAG, "🔧 流式播放任务退出时已停止I2S硬件");

    free(play_buffer);
    ESP_LOGI(TAG, "✅ 流式播放任务已完全退出");
    vTaskDelete(NULL);
}

void AudioManager::finish_streaming_playback() {
    if (!is_streaming) {
        return;
    }
    
    ESP_LOGI(TAG, "🎬 结束流式音频播放（千问方法）");
    
    // 🔇 立即停止I2S以防止重复播放
    bsp_audio_stop();
    
    // 🎬 处理最后的尾巴数据（不足一个块的部分）
    size_t remaining_data = streaming_write_pos - streaming_read_pos;
    
    if (remaining_data > 0) {
        ESP_LOGI(TAG, "🎹 播放尾巴数据: %zu 字节", remaining_data);
        // 分配临时缓冲区
        uint8_t* remaining_buffer = (uint8_t*)malloc(remaining_data);
        if (remaining_buffer) {
            // 读取所有尾巴数据
            memcpy(remaining_buffer, streaming_buffer + streaming_read_pos, remaining_data);
            
            // 🔇 关键：使用bsp_play_audio（普通版本，会自动停止I2S）
            esp_err_t ret = bsp_play_audio(remaining_buffer, remaining_data);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "✅ 尾巴音频播放完成并自动停止I2S，解决重复问题");
            } else {
                ESP_LOGE(TAG, "❌ 尾巴音频播放失败: %s", esp_err_to_name(ret));
                // 如果播放失败，手动停止I2S
                bsp_audio_stop();
            }
            
            free(remaining_buffer);
        }
    } else {
        // 没有尾巴数据，直接停止I2S
        bsp_audio_stop();
    }
    
    is_streaming = false;
    streaming_write_pos = 0;
    streaming_read_pos = 0;
    
    ESP_LOGI(TAG, "✅ 流式播放已完全结束，重复音频问题已解决");
}