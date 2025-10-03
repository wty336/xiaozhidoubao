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

#include "bsp_board.h"
#include "wifi_manager.h"
#include "websocket_client.h"
#include "audio_manager.h"
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

    // 初始化WiFi (需要提供参数)
    wifi_manager = new WiFiManager(CONFIG_EXAMPLE_WIFI_SSID, CONFIG_EXAMPLE_WIFI_PASSWORD);
    wifi_manager->connect();

    // 初始化WebSocket客户端
    ws_client = new WebSocketClient(CONFIG_EXAMPLE_WEBSOCKET_URI);
    ws_client->set_event_handler(on_websocket_event);
    ws_client->start(); // 启动连接任务

    // 初始化音频管理器
    audio_manager = new AudioManager(16000, 10, 32);

    // 初始化音频发送队列
    s_audio_send_queue = xQueueCreate(20, sizeof(AudioQueueItem));

    // 创建音频录制任务
    xTaskCreate(AudioManager::audio_record_task, "audio_record_task", 4 * 1024, audio_manager, 5, NULL);

    // 加载唤醒词模型
    srmodel_list_t *models = esp_srmodel_init("model");
    char *model_name = esp_srmodel_filter(models, SRMODELEXT_WAKENET, NULL);
    const esp_wn_iface_t *wakenet = esp_wn_handle_from_name(model_name);
    
    ESP_LOGI(TAG, "系统初始化完成，等待唤醒...");

    // 主循环
    while (true) {
        if (current_state == SpeechState::IDLE) {
            int keyword_index = esp_wn_detect(wakenet, NULL);
            if (keyword_index >= 0) {
                current_state = SpeechState::SESSION_ACTIVE;
                ESP_LOGI(TAG, "🎉 检测到唤醒词！");
                
                if (!ws_client->is_connected()) {
                    ESP_LOGI(TAG, "WebSocket未连接，正在连接...");
                    ws_client->connect();
                }

                audio_manager->play_audio(hi_mp3, sizeof(hi_mp3));
                audio_manager->start_recording();
                audio_manager->start_streaming_playback();
            }
        }

        // 检查音频队列并发送
        AudioQueueItem item;
        if (xQueueReceive(s_audio_send_queue, &item, 0) == pdTRUE) {
            if (ws_client->is_connected()) {
                ws_client->send_binary(item.data, item.len);
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
            current_state = SpeechState::IDLE;
            ESP_LOGI(TAG, "重置状态为空闲");
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
            ESP_LOGI(TAG, "收到WebSocket文本数据: %s", (char*)event.data);
            break;
    }
}