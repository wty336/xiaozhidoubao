/**
 * @file audio_manager.h
 * @brief 🎧 音频管理器头文件 (已修改为不再依赖OPUS)
 * * 声明了AudioManager类的接口，用于管理音频录制、播放和流式处理。
 */

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// 定义音频发送队列的结构体
struct AudioQueueItem {
    uint8_t* data;
    size_t len;
};

// 声明全局音频发送队列
extern QueueHandle_t s_audio_send_queue;

class AudioManager {
public:
    AudioManager(uint32_t sample_rate, uint32_t recording_duration_sec, uint32_t response_duration_sec);
    ~AudioManager();

    // 录音控制
    void start_recording();
    void stop_recording();

    // 播放控制
    esp_err_t play_audio(const uint8_t* data, size_t len);

    // 流式播放控制
    void start_streaming_playback();
    void stop_streaming_playback();
    void finish_streaming_playback();  // 新增：完成流式播放（千问方法）
    void feed_streaming_audio(const uint8_t* data, size_t len);

    // 静态任务函数
    static void audio_record_task(void *arg);

private:
    static const char* TAG;
    static const size_t STREAMING_BUFFER_SIZE = 64 * 1024; // 64KB 增大缓冲区防止溢出

    uint32_t sample_rate;
    uint32_t recording_duration_sec;
    uint32_t response_duration_sec;

    int16_t* recording_buffer;
    size_t recording_buffer_size;
    size_t recording_length;
    bool is_recording;

    int16_t* response_buffer;
    size_t response_buffer_size;
    size_t response_length;
    bool response_played;

    bool is_streaming;
    uint8_t* streaming_buffer;
    size_t streaming_buffer_size;
    volatile size_t streaming_write_pos;
    volatile size_t streaming_read_pos;

    static void streaming_playback_task(void* arg);
};

#endif // AUDIO_MANAGER_H