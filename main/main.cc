/**
 * @file main.cc
 * @brief 🎯 主程序入口 (最终简化版)
 *
 * 程序的启动点，负责初始化系统、连接网络、设置语音识别流程和处理用户交互。
 * 移除了所有本地命令处理和不再需要的复杂状态。
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_afe_sr_models.h"
#include "esp_process_sdkconfig.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"

#include "bsp_board.h"
#include "wifi_manager.h"
#include "websocket_client.h"
#include "audio_manager.h"
#include "project_config.h"  // 添加配置文件
#include "mock_voices/hi.h" // 导入提示音

static const char* TAG = "语音识别";

// 全局变量
static WiFiManager* wifi_manager = nullptr;
static WebSocketClient* ws_client = nullptr;
static AudioManager* audio_manager = nullptr;
QueueHandle_t s_audio_send_queue = nullptr;

// 语音识别状态
enum class SpeechState {
    IDLE,               // 空闲，等待唤醒
    SESSION_ACTIVE,     // 唤醒后，会话激活直到断开连接
};
static SpeechState current_state = SpeechState::IDLE;

// 全局变量：唤醒状态控制
static int wake_up_counter = 0;
static bool wake_up_triggered = false;

// 函数声明
void on_websocket_event(const WebSocketClient::EventData& event);

/**
 * @brief 主程序入口
 */
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "系统启动...");

    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 初始化硬件 (需要提供参数)
    bsp_board_init(16000, 1, 16);
    
    // 初始化音频播放功能
    ESP_LOGI(TAG, "初始化音频播放功能...");
    esp_err_t audio_init_ret = bsp_audio_init(16000, 1, 16);
    if (audio_init_ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 音频播放初始化失败: %s", esp_err_to_name(audio_init_ret));
    } else {
        ESP_LOGI(TAG, "✅ 音频播放初始化成功");
    }

    // 初始化WiFi (需要提供参数)
    wifi_manager = new WiFiManager(CONFIG_EXAMPLE_WIFI_SSID, CONFIG_EXAMPLE_WIFI_PASSWORD);
    esp_err_t wifi_ret = wifi_manager->connect();
    if (wifi_ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ WiFi连接失败，无法继续");
        // 这里可以选择重启或者进入離线模式
        while(1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            ESP_LOGE(TAG, "请检查WiFi配置: SSID=%s", CONFIG_EXAMPLE_WIFI_SSID);
        }
    }
    
    // 检查WiFi连接状态
    if (wifi_manager->isConnected()) {
        ESP_LOGI(TAG, "✅ WiFi连接成功，IP地址: %s", wifi_manager->getIpAddress().c_str());
    } else {
        ESP_LOGE(TAG, "❌ WiFi连接失败");
    }

    // 初始化WebSocket客户端并立即连接
    ws_client = new WebSocketClient(CONFIG_EXAMPLE_WEBSOCKET_URI);
    ws_client->setEventCallback(on_websocket_event);
    
    // 立即尝试连接WebSocket，避免唤醒时才连接导致音频丢失
    ESP_LOGI(TAG, "🌐 正在连接WebSocket服务器...");
    esp_err_t ws_ret = ws_client->connect();
    if (ws_ret != ESP_OK) {
        ESP_LOGW(TAG, "⚠️ 初始WebSocket连接失败，将在唤醒时重试");
    } else {
        // 等待连接建立
        int connect_wait = 0;
        while (!ws_client->isConnected() && connect_wait < 150) {  // 增加等待时间到15秒
            vTaskDelay(pdMS_TO_TICKS(100));
            connect_wait++;
        }
        if (ws_client->isConnected()) {
            ESP_LOGI(TAG, "✅ WebSocket连接成功，准备就绪");
        } else {
            ESP_LOGW(TAG, "⚠️ WebSocket连接超时，将在唤醒时重试");
        }
    }

    // 初始化音频管理器
    audio_manager = new AudioManager(16000, 10, 32);

    // 初始化音频发送队列
    s_audio_send_queue = xQueueCreate(20, sizeof(AudioQueueItem));

    // 创建音频录制任务
    xTaskCreate(AudioManager::audio_record_task, "audio_record_task", 4 * 1024, audio_manager, 5, NULL);

    // 加载唤醒词模型
    ESP_LOGI(TAG, "正在初始化唤醒词检测...");
    srmodel_list_t *models = esp_srmodel_init("model");
    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    const esp_wn_iface_t *wakenet = esp_wn_handle_from_name(model_name);
    
    if (wakenet && model_name) {
        ESP_LOGI(TAG, "✅ 唤醒词模型加载成功: %s", model_name);
        char *wake_word = esp_wn_wakeword_from_name(model_name);
        if (wake_word) {
            ESP_LOGI(TAG, "✅ 支持的唤醒词: %s", wake_word);
        }
    } else {
        ESP_LOGW(TAG, "⚠️ 唤醒词模型未找到，使用测试模式");
        wakenet = nullptr;
    }
    
    ESP_LOGI(TAG, "系统初始化完成，等待唤醒...");
    ESP_LOGI(TAG, "💡 调试信息:");
    ESP_LOGI(TAG, "   - WiFi SSID: %s", CONFIG_EXAMPLE_WIFI_SSID);
    ESP_LOGI(TAG, "   - WebSocket URI: %s", CONFIG_EXAMPLE_WEBSOCKET_URI);
    ESP_LOGI(TAG, "   - 自动唤醒间隔: 30秒（仅用于测试）");
    ESP_LOGI(TAG, "   - 如需修改配置，请编辑 main/project_config.h");

    // 主循环 - 支持真正的唤醒词检测
    model_iface_data_t *model_data = nullptr;
    int16_t *audio_buffer = nullptr;
    int audio_chunksize = 0;
    
    // 初始化唤醒词检测模型
    if (wakenet) {
        model_data = wakenet->create(model_name, DET_MODE_90);
        if (model_data) {
            audio_chunksize = wakenet->get_samp_chunksize(model_data);
            audio_buffer = (int16_t*)malloc(audio_chunksize * sizeof(int16_t));
            ESP_LOGI(TAG, "✅ 唤醒词检测初始化成功，缓冲区大小: %d", audio_chunksize);
        }
    }
    
    while (true) {
        if (current_state == SpeechState::IDLE) {
            if (wakenet && model_data && audio_buffer) {
                // 真正的唤醒词检测
                esp_err_t ret = bsp_get_feed_data(false, audio_buffer, audio_chunksize * sizeof(int16_t));
                if (ret == ESP_OK) {
                    wakenet_state_t wake_state = wakenet->detect(model_data, audio_buffer);
                    if (wake_state == WAKENET_DETECTED) {
                        current_state = SpeechState::SESSION_ACTIVE;
                        ESP_LOGI(TAG, "🎉 检测到唤醒词！");
                        
                        // 停止可能存在的录音任务
                        audio_manager->stop_recording();
                        
                        // 确保WebSocket连接
                        if (!ws_client->isConnected()) {
                            ESP_LOGI(TAG, "WebSocket未连接，正在重新连接...");
                            ws_client->disconnect();  // 清理可能存在的旧连接
                            vTaskDelay(pdMS_TO_TICKS(100));
                            esp_err_t conn_ret = ws_client->connect();
                            
                            if (conn_ret != ESP_OK) {
                                ESP_LOGE(TAG, "❌ WebSocket连接初始化失败: %s", esp_err_to_name(conn_ret));
                            } else {
                                // 等待连接建立，增加等待时间
                                int connect_retry = 0;
                                while (!ws_client->isConnected() && connect_retry < 50) {  // 等待最多5秒
                                    vTaskDelay(pdMS_TO_TICKS(100));
                                    connect_retry++;
                                }
                            }
                        }
                        
                        if (ws_client->isConnected()) {
                            audio_manager->play_audio(hi_mp3, hi_mp3_len);
                            vTaskDelay(pdMS_TO_TICKS(500)); // 等待提示音播放完成
                            audio_manager->start_recording();
                            audio_manager->start_streaming_playback();
                        } else {
                            ESP_LOGE(TAG, "❌ WebSocket连接失败，返回空闲状态");
                            current_state = SpeechState::IDLE;
                        }
                    }
                }
            } else {
                // 备用测试模式 - 每30秒自动唤醒
                wake_up_counter++;
                if (wake_up_counter >= 3000 && !wake_up_triggered) { // 30秒 (3000 * 10ms)
                    wake_up_triggered = true;
                    current_state = SpeechState::SESSION_ACTIVE;
                    ESP_LOGI(TAG, "🎉 测试模式自动唤醒！");
                    
                    // 确保WebSocket连接
                    if (!ws_client->isConnected()) {
                        ESP_LOGI(TAG, "WebSocket未连接，正在重新连接...");
                        ws_client->disconnect();  // 清理可能存在的旧连接
                        vTaskDelay(pdMS_TO_TICKS(100));
                        ws_client->connect();
                        
                        // 等待连接建立，但不要等太久
                        int connect_retry = 0;
                        while (!ws_client->isConnected() && connect_retry < 30) {
                            vTaskDelay(pdMS_TO_TICKS(100));
                            connect_retry++;
                        }
                    }
                    
                    if (ws_client->isConnected()) {
                        audio_manager->play_audio(hi_mp3, hi_mp3_len);
                        vTaskDelay(pdMS_TO_TICKS(500)); // 等待提示音播放完成
                        audio_manager->start_recording();
                        audio_manager->start_streaming_playback();
                    } else {
                        ESP_LOGE(TAG, "❌ WebSocket连接失败，返回空闲状态");
                        current_state = SpeechState::IDLE;
                        wake_up_triggered = false;
                        wake_up_counter = 0;
                    }
                }
            }
        }

        // 检查音频队列并发送
        AudioQueueItem item;
        if (xQueueReceive(s_audio_send_queue, &item, 0) == pdTRUE) {
            if (ws_client->isConnected()) {
                int sent = ws_client->sendBinary(item.data, item.len);
                if (sent < 0) {
                    ESP_LOGW(TAG, "⚠️ 发送音频数据失败");
                }
            } else {
                ESP_LOGW(TAG, "⚠️ WebSocket未连接，丢弃音频数据");
            }
            free(item.data); // 释放内存
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief WebSocket事件回调
 */
void on_websocket_event(const WebSocketClient::EventData& event) {
    switch (event.type) {
        case WebSocketClient::EventType::CONNECTED:
            ESP_LOGI(TAG, "🔗 WebSocket已连接");
            break;
        case WebSocketClient::EventType::DISCONNECTED:
            ESP_LOGI(TAG, "🔌 WebSocket已断开");
            if (audio_manager) {
                audio_manager->stop_recording();
                audio_manager->stop_streaming_playback();
            }
            
            // 如果是在会话活跃状态下断开，尝试重连一次
            if (current_state == SpeechState::SESSION_ACTIVE) {
                ESP_LOGI(TAG, "🔄 会话期间连接断开，尝试重连...");
                vTaskDelay(pdMS_TO_TICKS(1000)); // 等待1秒后重试
                ws_client->disconnect();  // 确保清理旧连接
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_err_t conn_ret = ws_client->connect();
                
                if (conn_ret != ESP_OK) {
                    ESP_LOGE(TAG, "❌ WebSocket重连初始化失败: %s", esp_err_to_name(conn_ret));
                } else {
                    // 等待重连结果
                    int reconnect_retry = 0;
                    while (!ws_client->isConnected() && reconnect_retry < 50) {  // 等待最多5秒
                        vTaskDelay(pdMS_TO_TICKS(100));
                        reconnect_retry++;
                    }
                }
                
                if (!ws_client->isConnected()) {
                    ESP_LOGE(TAG, "❌ 重连失败，返回空闲状态");
                    current_state = SpeechState::IDLE;
                    wake_up_triggered = false;
                    wake_up_counter = 0;
                    // 停止录音
                    audio_manager->stop_recording();
                } else {
                    ESP_LOGI(TAG, "✅ 重连成功，继续会话");
                    // 重新开始录音和播放
                    audio_manager->start_recording();
                    audio_manager->start_streaming_playback();
                }
            } else {
                current_state = SpeechState::IDLE;
                ESP_LOGI(TAG, "重置状态为空闲");
                wake_up_triggered = false;
                wake_up_counter = 0;
                // 停止录音
                audio_manager->stop_recording();
            }
            break;
        case WebSocketClient::EventType::ERROR:
            ESP_LOGE(TAG, "❌ WebSocket错误");
            break;
        case WebSocketClient::EventType::DATA_BINARY:
            if (audio_manager) {
                audio_manager->feed_streaming_audio(event.data, event.data_len);
            }
            break;
        case WebSocketClient::EventType::DATA_TEXT:
            ESP_LOGI(TAG, "💬 收到WebSocket文本数据: %s", (char*)event.data);
            // 🔇 检测是否是明确的TTS结束信号
            if (strstr((char*)event.data, "\"type\":\"tts_end\"")) {
                ESP_LOGI(TAG, "🔇 检测到TTS结束信号，调用千问方法结束播放");
                if (audio_manager) {
                    ESP_LOGI(TAG, "🎬 调用finish_streaming_playback()结束流式播放...");
                    // 增加一个小延迟确保所有音频数据处理完成
                    vTaskDelay(pdMS_TO_TICKS(50));
                    audio_manager->finish_streaming_playback();
                    ESP_LOGI(TAG, "✅ 流式播放已结束");
                }
            }
            break;
        case WebSocketClient::EventType::PING:
            ESP_LOGI(TAG, "收到WebSocket ping");
            break;
        case WebSocketClient::EventType::PONG:
            ESP_LOGI(TAG, "收到WebSocket pong");
            break;
    }
}