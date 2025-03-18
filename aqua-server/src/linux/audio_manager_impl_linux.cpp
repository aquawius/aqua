//
// Created by aquawius on 25-1-11.
//

#include "audio_manager_impl_linux.h"
#include "version.h"

#include <spdlog/spdlog.h>

#ifdef __linux__

#include <spa/utils/result.h>

// PipeWire回调声明
void on_process(void* userdata);
void on_stream_process_cb(void* userdata);
void on_stream_state_changed_cb(void* userdata, pw_stream_state, pw_stream_state, const char*);

static constexpr struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed_cb,
    .process = on_stream_process_cb
};

audio_manager_impl_linux::audio_manager_impl_linux()
{
    // 初始化流配置
    m_stream_config.sample_rate = 48000;
    m_stream_config.channels = 2;
    m_stream_config.encoding = AudioEncoding::PCM_F32LE;
    m_stream_config.bit_depth = 32; // 默认使用32位浮点
    m_latency = 1024; // PipeWire 特有字段
    
    spdlog::debug("[Linux] Audio manager instance created.");
}

audio_manager_impl_linux::~audio_manager_impl_linux()
{
    stop_capture(); // 确保先停止捕获

    // 按顺序释放资源：流 → 上下文 → 主循环
    if (p_stream) {
        spdlog::debug("Destroying PipeWire stream...");
        pw_stream_destroy(p_stream);
        p_stream = nullptr;
    }

    if (p_context) {
        spdlog::debug("Destroying PipeWire context...");
        pw_context_destroy(p_context);
        p_context = nullptr;
    }

    if (p_main_loop) {
        spdlog::debug("Destroying PipeWire main loop...");
        pw_main_loop_destroy(p_main_loop);
        p_main_loop = nullptr;
    }

    pw_deinit();
    spdlog::info("[Linux] Audio manager destroyed.");
}

void log_pipewire_debug_info()
{
    spdlog::debug("PipeWire Header Version: {}",
        pw_get_client_name() ? pw_get_client_name() : "<unknown>");
    spdlog::debug("PipeWire Library Version: {}",
        pw_get_library_version() ? pw_get_library_version() : "<unknown>");
    spdlog::debug("Application Name: {}",
        pw_get_application_name() ? pw_get_application_name() : "<unknown>");
    spdlog::debug("Program Name: {}",
        pw_get_prgname() ? pw_get_prgname() : "<unknown>");
    spdlog::debug("User Name: {}",
        pw_get_user_name() ? pw_get_user_name() : "<unknown>");
    spdlog::debug("Host Name: {}",
        pw_get_host_name() ? pw_get_host_name() : "<unknown>");
    spdlog::debug("Client Name: {}",
        pw_get_client_name() ? pw_get_client_name() : "<unknown>");
}

// 初始化
bool audio_manager_impl_linux::init()
{
    pw_init(nullptr, nullptr);
    spdlog::info("[Linux] PipeWire initialized (version: {})", pw_get_library_version());

    p_main_loop = pw_main_loop_new(nullptr);
    if (!p_main_loop) {
        spdlog::critical("[Linux] Failed to create PipeWire main loop.");
        return false;
    }

    // 创建上下文
    p_context = pw_context_new(pw_main_loop_get_loop(p_main_loop), nullptr, 0);
    if (!p_context) {
        spdlog::critical("[Linux] Failed to create PipeWire context.");
        return false;
    }

    return true;
}

// 流配置
bool audio_manager_impl_linux::setup_stream()
{
    if (!p_main_loop || !p_context) {
        spdlog::error("[Linux] setup_stream() failed: PipeWire not initialized.");
        return false;
    }

    // 配置流参数 - 使用自定义的latency字段
    const std::string latency_str = std::to_string(m_latency) + "/" + std::to_string(m_stream_config.sample_rate);
    const std::string stream_name = std::string(aqua_server_BINARY_NAME) + "-capture";

    // 创建流属性（优化日志和参数）
    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_STREAM_CAPTURE_SINK, "true", // 捕获扬声器输出
        PW_KEY_NODE_NAME, stream_name.c_str(),
        PW_KEY_NODE_LATENCY, latency_str.c_str(),
        nullptr);

    // 创建流
    p_stream = pw_stream_new_simple(
        pw_main_loop_get_loop(p_main_loop), // loop
        stream_name.c_str(), // name
        props, // properties
        &stream_events, // events
        this // userdata
        );

    if (!p_stream) {
        spdlog::error("[Linux] Failed to create PipeWire stream.");
        return false;
    }

    // 配置音频格式
    m_builder = SPA_POD_BUILDER_INIT(m_buffer, sizeof(m_buffer));
    spa_audio_info_raw audio_info = {
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = m_stream_config.sample_rate,
        .channels = m_stream_config.channels
    };
    p_params[0] = spa_format_audio_raw_build(&m_builder, SPA_PARAM_EnumFormat, &audio_info);

    // 连接流
    int ret = pw_stream_connect(
        p_stream,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_RT_PROCESS),
        p_params,
        1);

    if (ret < 0) {
        spdlog::error("[Linux] Stream connection failed: {} (code: {})", spa_strerror(ret), ret);
        return false;
    }

    spdlog::info("[Linux] Stream configured: {} Hz, {} channels",
        m_stream_config.sample_rate, m_stream_config.channels);
    return true;
}

// 捕获控制
bool audio_manager_impl_linux::start_capture(const AudioDataCallback& callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_is_capturing) {
        spdlog::warn("[Linux] Capture is already running.");
        return false;
    }

    set_data_callback(callback);
    m_promise_initialized = std::promise<void>();

    // 启动捕获线程（使用stop_token确保安全退出）
    m_capture_thread = std::jthread([this](std::stop_token stop_token) {
        m_is_capturing = true;
        m_promise_initialized.set_value();

        // 设置停止回调
        std::stop_callback stop_cb(stop_token, [this]() {
            if (p_main_loop)
                pw_main_loop_quit(p_main_loop);
        });

        // 运行主循环
        spdlog::info("[Linux] Starting PipeWire main loop...");
        pw_main_loop_run(p_main_loop);
        spdlog::info("[Linux] PipeWire main loop exited.");

        m_is_capturing = false;
    });

    // 等待线程初始化完成
    m_promise_initialized.get_future().wait();
    return true;
}

bool audio_manager_impl_linux::stop_capture()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_is_capturing) {
        spdlog::warn("[Linux] No active capture to stop.");
        return false;
    }

    // 请求线程停止
    m_capture_thread.request_stop();
    if (p_main_loop)
        pw_main_loop_quit(p_main_loop);

    // 等待线程退出
    if (m_capture_thread.joinable()) {
        m_capture_thread.join();
        spdlog::debug("[Linux] Capture thread joined.");
    }

    return true;
}

bool audio_manager_impl_linux::is_capturing() const
{
    return m_is_capturing;
}

const audio_manager_impl_linux::stream_config& audio_manager_impl_linux::get_format() const
{
    return m_stream_config;
}

void audio_manager_impl_linux::set_data_callback(AudioDataCallback callback)
{
    m_data_callback = std::move(callback);
}

void audio_manager_impl_linux::set_peak_callback(AudioPeakCallback callback)
{
    m_peak_callback = std::move(callback);
}

// 数据处理
void audio_manager_impl_linux::process_audio_buffer(std::span<const std::byte> audio_buffer) const
{
    if (audio_buffer.empty())
        return;

    if (m_data_callback) {
        // 直接传递字节数据给网络层，不做任何转换
        m_data_callback(audio_buffer);
    }

    // 对于音量峰值计算，如果原始数据不是浮点格式，需要先转换
    // 这里假设数据是按照配置的格式编码的
    const auto& format = get_preferred_format();
    std::vector<float> float_buffer;
    
    // 只有当需要计算峰值且有回调时才进行转换
    if (m_peak_callback) {
        size_t samples_count = 0;
        
        // 计算样本数量
        switch (format.encoding) {
        case AudioEncoding::PCM_S16LE:
            samples_count = audio_buffer.size() / sizeof(int16_t);
            break;
        case AudioEncoding::PCM_S24LE:
            samples_count = audio_buffer.size() / 3; // 24位占3字节
            break;
        case AudioEncoding::PCM_S32LE:
            samples_count = audio_buffer.size() / sizeof(int32_t);
            break;
        case AudioEncoding::PCM_F32LE:
            samples_count = audio_buffer.size() / sizeof(float);
            break;
        case AudioEncoding::PCM_U8:
            samples_count = audio_buffer.size() / sizeof(uint8_t);
            break;
        default:
            return; // 无法处理未知格式
        }
        
        // 为峰值检测转换数据到浮点格式
        float_buffer.resize(samples_count);
        
        switch (format.encoding) {
        case AudioEncoding::PCM_S16LE: {
            const int16_t* src = reinterpret_cast<const int16_t*>(audio_buffer.data());
            for (size_t i = 0; i < samples_count; ++i) {
                float_buffer[i] = src[i] / 32767.0f;
            }
            break;
        }
        case AudioEncoding::PCM_S24LE: {
            for (size_t i = 0; i < samples_count; ++i) {
                // 从3字节组装24位有符号整数 (小端序)
                int32_t sample = static_cast<int32_t>(audio_buffer[i * 3]) |
                                (static_cast<int32_t>(audio_buffer[i * 3 + 1]) << 8) |
                                (static_cast<int32_t>(audio_buffer[i * 3 + 2]) << 16);
                // 如果最高位是1，进行符号扩展
                if (sample & 0x800000) {
                    sample |= 0xFF000000;
                }
                float_buffer[i] = sample / 8388607.0f; // 2^23-1
            }
            break;
        }
        case AudioEncoding::PCM_S32LE: {
            const int32_t* src = reinterpret_cast<const int32_t*>(audio_buffer.data());
            for (size_t i = 0; i < samples_count; ++i) {
                float_buffer[i] = src[i] / 2147483647.0f;
            }
            break;
        }
        case AudioEncoding::PCM_U8: {
            const uint8_t* src = reinterpret_cast<const uint8_t*>(audio_buffer.data());
            for (size_t i = 0; i < samples_count; ++i) {
                float_buffer[i] = (src[i] / 255.0f) * 2.0f - 1.0f;
            }
            break;
        }
        case AudioEncoding::PCM_F32LE: {
            // 直接复制浮点数据
            std::memcpy(float_buffer.data(), audio_buffer.data(), samples_count * sizeof(float));
            break;
        }
        default:
            return; // 无法处理未知格式
        }
        
        // 计算峰值
        process_volume_peak(std::span<const float>(float_buffer));
    }
}

void audio_manager_impl_linux::process_volume_peak(const std::span<const float> data) const
{
    if (data.empty()) {
        return;
    }

    const size_t size = data.size();
    float local_peak = 0.0f;

    // 均匀采样点树
    constexpr size_t SAMPLE_POINTS = 30;
    for (size_t i = 0; i < SAMPLE_POINTS; ++i) {
        // 计算采样位置：从0到size-1均匀分布
        const size_t index = (i * (size - 1)) / (SAMPLE_POINTS - 1);
        local_peak = std::max(local_peak, std::abs(data[index]));
    }

    if (m_peak_callback) {
        m_peak_callback(local_peak);
    }
}

// PipeWire回调函数
void on_process(void* userdata)
{
    auto* mgr = static_cast<audio_manager_impl_linux*>(userdata);
    if (!mgr || !mgr->p_stream) {
        spdlog::warn("[Linux] Invalid manager or stream in callback.");
        return;
    }

    std::lock_guard<std::mutex> lock(mgr->m_mutex);
    if (!mgr->m_is_capturing)
        return;

    struct pw_buffer* buffer = pw_stream_dequeue_buffer(mgr->p_stream);
    if (!buffer || !buffer->buffer || !buffer->buffer->datas[0].data) {
        spdlog::warn("[Linux] Invalid buffer received.");
        return;
    }

    // 获取数据的原始字节指针
    const auto* data = static_cast<const std::byte*>(buffer->buffer->datas[0].data);
    const uint32_t n_bytes = buffer->buffer->datas[0].chunk->size;
    
    // 传递字节数据，不进行任何类型转换
    mgr->process_audio_buffer(std::span<const std::byte>(data, n_bytes));

    pw_stream_queue_buffer(mgr->p_stream, buffer);
}

void on_stream_state_changed_cb(void* userdata, const pw_stream_state old, const pw_stream_state state, const char* error)
{
    if (error) {
        spdlog::error("[Linux] Stream error: {}", error);
    }
    spdlog::info("[Linux] Stream state changed: {} -> {}",
        pw_stream_state_as_string(old),
        pw_stream_state_as_string(state));
}

void on_stream_process_cb(void* userdata)
{
    on_process(userdata);
}

// PipeWire格式转换为AudioEncoding
audio_manager::AudioEncoding audio_manager_impl_linux::spa_format_to_encoding(spa_audio_format format)
{
    switch (format) {
    case SPA_AUDIO_FORMAT_F32:
    case SPA_AUDIO_FORMAT_F32_LE:
        return AudioEncoding::PCM_F32LE;
    case SPA_AUDIO_FORMAT_S16:
    case SPA_AUDIO_FORMAT_S16_LE:
        return AudioEncoding::PCM_S16LE;
    case SPA_AUDIO_FORMAT_S24:
    case SPA_AUDIO_FORMAT_S24_LE:
        return AudioEncoding::PCM_S24LE;
    case SPA_AUDIO_FORMAT_S32:
    case SPA_AUDIO_FORMAT_S32_LE:
        return AudioEncoding::PCM_S32LE;
    case SPA_AUDIO_FORMAT_U8:
        return AudioEncoding::PCM_U8;
    default:
        return AudioEncoding::INVALID;
    }
}

// 获取首选音频格式
audio_manager::AudioFormat audio_manager_impl_linux::get_preferred_format() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // 构建格式信息
    AudioFormat format;
    format.sample_rate = m_stream_config.sample_rate;
    format.channels = m_stream_config.channels;
    format.encoding = m_stream_config.encoding;
    
    // 根据编码格式设置位深度
    switch (format.encoding) {
    case AudioEncoding::PCM_S16LE:
        format.bit_depth = 16;
        break;
    case AudioEncoding::PCM_S24LE:
        format.bit_depth = 24;
        break;
    case AudioEncoding::PCM_S32LE:
    case AudioEncoding::PCM_F32LE:
        format.bit_depth = 32;
        break;
    case AudioEncoding::PCM_U8:
        format.bit_depth = 8;
        break;
    default:
        format.bit_depth = 32; // 默认32位
        break;
    }
    
    return format;
}

// 获取支持的编码格式列表
std::vector<audio_manager::AudioEncoding> audio_manager_impl_linux::get_supported_formats() const
{
    // PipeWire支持多种格式
    return {
        AudioEncoding::PCM_F32LE,  // 浮点32位小端序
        AudioEncoding::PCM_S16LE,  // 带符号16位小端序 
        AudioEncoding::PCM_S24LE,  // 带符号24位小端序
        AudioEncoding::PCM_S32LE,  // 带符号32位小端序
        AudioEncoding::PCM_U8      // 无符号8位
    };
}

#endif // __linux__