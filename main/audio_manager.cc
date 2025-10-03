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
        bsp_record_start();
    }
}

void AudioManager::stop_recording() {
    if (is_recording) {
        is_recording = false;
        bsp_record_stop();
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

        bsp_get_feed_data(pcm_data, pcm_data_size);

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
                xQueueSend(s_audio_send_queue, &item, portMAX_DELAY);
            }
        }
    }
    free(pcm_data);
    vTaskDelete(NULL);
}


void AudioManager::start_streaming_playback() {
    is_streaming = true;
    streaming_write_pos = 0;
    streaming_read_pos = 0;
    xTaskCreate(streaming_playback_task, "streaming_playback_task", 4096, this, 5, NULL);
}

void AudioManager::stop_streaming_playback() {
    is_streaming = false;
}

void AudioManager::feed_streaming_audio(const uint8_t* data, size_t len) {
    if (!is_streaming) return;

    size_t space_available;
    if (streaming_write_pos >= streaming_read_pos) {
        space_available = streaming_buffer_size - (streaming_write_pos - streaming_read_pos);
    } else {
        space_available = streaming_read_pos - streaming_write_pos;
    }

    if (len > space_available) {
        ESP_LOGW(TAG, "流式播放缓冲区已满，丢弃 %zu 字节", len);
        return;
    }

    if (streaming_write_pos + len <= streaming_buffer_size) {
        memcpy(streaming_buffer + streaming_write_pos, data, len);
        streaming_write_pos += len;
    } else {
        size_t first_chunk_size = streaming_buffer_size - streaming_write_pos;
        memcpy(streaming_buffer + streaming_write_pos, data, first_chunk_size);
        size_t second_chunk_size = len - first_chunk_size;
        memcpy(streaming_buffer, data + first_chunk_size, second_chunk_size);
        streaming_write_pos = second_chunk_size;
    }
}

void AudioManager::streaming_playback_task(void* arg) {
    AudioManager* self = (AudioManager*)arg;
    const size_t play_chunk_size = 200 * (self->sample_rate / 1000) * sizeof(int16_t);
    uint8_t* play_buffer = (uint8_t*)malloc(play_chunk_size);

    ESP_LOGI(TAG, "开始流式音频播放");

    while (self->is_streaming) {
        size_t data_available;
        if (self->streaming_write_pos >= self->streaming_read_pos) {
            data_available = self->streaming_write_pos - self->streaming_read_pos;
        } else {
            data_available = self->streaming_buffer_size - self->streaming_read_pos + self->streaming_write_pos;
        }

        if (data_available >= play_chunk_size) {
            if (self->streaming_read_pos + play_chunk_size <= self->streaming_buffer_size) {
                memcpy(play_buffer, self->streaming_buffer + self->streaming_read_pos, play_chunk_size);
                self->streaming_read_pos += play_chunk_size;
            } else {
                size_t first_chunk_size = self->streaming_buffer_size - self->streaming_read_pos;
                memcpy(play_buffer, self->streaming_buffer + self->streaming_read_pos, first_chunk_size);
                size_t second_chunk_size = play_chunk_size - first_chunk_size;
                memcpy(play_buffer + first_chunk_size, self->streaming_buffer, second_chunk_size);
                self->streaming_read_pos = second_chunk_size;
            }
            bsp_play_audio_streaming(play_buffer, play_chunk_size);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ESP_LOGI(TAG, "结束流式音频播放");
    
    size_t remaining_data;
    if (self->streaming_write_pos >= self->streaming_read_pos) {
        remaining_data = self->streaming_write_pos - self->streaming_read_pos;
    } else {
        remaining_data = self->streaming_buffer_size - self->streaming_read_pos + self->streaming_write_pos;
    }
    
    if (remaining_data > 0) {
        uint8_t* remaining_buffer = (uint8_t*)malloc(remaining_data);
        if (remaining_buffer) {
            if (self->streaming_write_pos >= self->streaming_read_pos) {
                memcpy(remaining_buffer, self->streaming_buffer + self->streaming_read_pos, remaining_data);
            } else {
                size_t bytes_to_end = self->streaming_buffer_size - self->streaming_read_pos;
                memcpy(remaining_buffer, self->streaming_buffer + self->streaming_read_pos, bytes_to_end);
                memcpy(remaining_buffer + bytes_to_end, self->streaming_buffer, self->streaming_write_pos);
            }
            
            bsp_play_audio(remaining_buffer, remaining_data);
            free(remaining_buffer);
        }
    }

    free(play_buffer);
    vTaskDelete(NULL);
}