//
// Created by aquawius on 25-1-11.
//

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class audio_manager {
public:
    // 基础结构定义
    struct capture_config {
        int sampleRate;
        int channels;
        int bitsPerSample;
        int framesPerBuffer;
        std::string format;
        bool isInterleaved;
    };

    struct AudioStats {
        double latency;
        int dropouts;
        int overflow_count;
    };

    struct AudioError {
    };

    enum class AudioState {
        Uninitialized,
        Initialized,
        Running,
        Paused,
        Stopped,
        Error
    };

    // 回调定义
    using AudioDataCallback = std::function<void(const std::vector<float>& audioData)>;
    using StateCallback = std::function<void(AudioState)>;
    using ErrorCallback = std::function<void(AudioError)>;

    // 工厂方法
    static std::unique_ptr<audio_manager> create(const std::string& backend = "default");

    virtual ~audio_manager() = default;

    // 初始化和控制
    virtual bool initialize(const capture_config& config) = 0;
    virtual bool start_capture(AudioDataCallback callback) = 0;
    virtual bool stop_capture() = 0;
    virtual bool pause_capture() = 0;
    virtual bool resume_capture() = 0;
    virtual bool is_capturing() const = 0;
    virtual void release() = 0;

    // 设备管理
    virtual std::vector<std::string> list_input_devices() const = 0;
    virtual std::vector<std::string> list_output_devices() const = 0;
    virtual bool set_input_device(const std::string& deviceId) = 0;
    virtual bool set_output_device(const std::string& deviceId) = 0;

    // 音量控制
    virtual bool set_volume(float volume) = 0;
    virtual float get_volume() const = 0;
    virtual bool set_mute(bool mute) = 0;
    virtual bool is_muted() const = 0;

    // 状态和错误处理
    virtual AudioState get_state() const = 0;
    virtual AudioError get_error_code() const = 0;
    virtual std::string get_last_error() const = 0;

    // 回调设置
    virtual void set_state_callback(StateCallback callback) = 0;
    virtual void set_error_callback(ErrorCallback callback) = 0;

    // 音频处理
    virtual bool enable_noise_suppression(bool enable) = 0;
    virtual bool enable_echo_cancellation(bool enable) = 0;

    // 性能监控
    virtual AudioStats get_stats() const = 0;

protected:
    mutable std::mutex m_mutex;
};
#endif // AUDIO_MANAGER_H
