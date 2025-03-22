//
// Created by aquawius on 25-1-11.
//

#include "audio_manager_impl_linux.h"
#include "version.h"

#include <spdlog/spdlog.h>

#ifdef __linux__

#include <spa/utils/result.h>

// PipeWire回调声明
void on_stream_process_cb(void* userdata);
void on_stream_state_changed_cb(void* userdata, pw_stream_state, pw_stream_state, const char*);

static constexpr struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed_cb,
    .process = on_stream_process_cb
};

audio_manager_impl_linux::audio_manager_impl_linux()
{
    spdlog::debug("[Linux] Audio manager instance created.");
}

audio_manager_impl_linux::~audio_manager_impl_linux()
{
    spdlog::debug("[Linux] Audio manager destructor called.");

    // 停止捕获
    stop_capture();

    // 按顺序释放资源：流 → 上下文 → 主循环
    if (p_stream) {
        spdlog::debug("[Linux] Destroying PipeWire stream...");
        pw_stream_destroy(p_stream);
        p_stream = nullptr;
    }

    if (p_context) {
        spdlog::debug("[Linux] Destroying PipeWire context...");
        pw_context_destroy(p_context);
        p_context = nullptr;
    }

    if (p_main_loop) {
        spdlog::debug("[Linux] Destroying PipeWire main loop...");
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
bool audio_manager_impl_linux::setup_stream(const AudioFormat format)
{
    if (!p_main_loop || !p_context) {
        spdlog::error("[Linux] setup_stream() failed: PipeWire not initialized.");
        return false;
    }

    m_pw_stream_latency = 1024; // PipeWire 特有字段

    // 保存格式配置
    if (!AudioFormat::is_valid(format)) {
        spdlog::error("[Linux] setup_stream() failed: Invalid audio format.");
        return false;
    }
    m_stream_config = format;

    // 配置流参数 - 使用自定义的latency字段
    const std::string latency_str = std::to_string(m_pw_stream_latency) + "/" + std::to_string(m_stream_config.sample_rate);
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

    // 获取传递的流参数中的，音频格式到SPA类型
    const spa_audio_format spa_format = convert_AudioEncoding_to_spa_audio_format(m_stream_config.encoding);

    // 配置音频格式
    m_builder = SPA_POD_BUILDER_INIT(m_buffer, sizeof(m_buffer));
    spa_audio_info_raw audio_info = {
        .format = spa_format,
        .rate = m_stream_config.sample_rate,
        .channels = m_stream_config.channels
    };

    p_params[0] = spa_format_audio_raw_build(&m_builder, SPA_PARAM_EnumFormat, &audio_info);

    // 连接流
    int ret = pw_stream_connect(
        p_stream,
        PW_DIRECTION_INPUT, // 捕获
        PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_RT_PROCESS),
        p_params,
        1);

    if (ret < 0) {
        spdlog::error("[Linux] Stream connection failed: {} (code: {})", spa_strerror(ret), ret);
        return false;
    }

    spdlog::info("[Linux] Stream configured: {} Hz, {} ch, {} bit, {}",
        m_stream_config.sample_rate,
        m_stream_config.channels,
        m_stream_config.bit_depth,
        AudioFormat::is_float_encoding(format.encoding).value_or(false) ? "float" : "int");

    return true;
}

bool audio_manager_impl_linux::reconfigure_stream(const AudioFormat& new_format)
{
    bool was_capturing = false;

    AudioDataCallback saved_data_callback;
    AudioPeakCallback saved_peak_callback;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (new_format == m_stream_config) {
            return true;
        }

        was_capturing = m_is_capturing;

        // 分别保存两种回调
        saved_data_callback = m_data_callback;
        saved_peak_callback = m_peak_callback;

        spdlog::info("[Linux] Stream Reconfiguring: {} Hz, {} ch, {} bit, {}",
            m_stream_config.sample_rate,
            m_stream_config.channels,
            m_stream_config.bit_depth,
            AudioFormat::is_float_encoding(new_format.encoding).value_or(false) ? "float" : "int");
    }

    // 停止捕获
    if (!stop_capture()) {
        spdlog::error("[Linux] Failed to stop stream during reconfig.");
        return false;
    }

    // 销毁旧流
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (p_stream) {
            pw_stream_disconnect(p_stream);
            pw_stream_destroy(p_stream);
            p_stream = nullptr;
        }
        m_stream_config = new_format;
    }

    // 创建新流
    if (!setup_stream(new_format)) {
        spdlog::error("[Linux] Failed to setup new stream.");
        return false;
    }

    // 重启捕获并恢复回调
    if (was_capturing) {
        if (!start_capture(saved_data_callback)) {
            spdlog::error("[Linux] Failed to restart data capture.");
            return false;
        }
        set_peak_callback(saved_peak_callback); // 单独恢复峰值回调
    }

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

    set_data_callback(nullptr);
    set_peak_callback(nullptr);

    return true;
}

bool audio_manager_impl_linux::is_capturing() const
{
    return m_is_capturing;
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
        // 直接传递原始数据给回调函数
        m_data_callback(audio_buffer);
    }

    // 峰值计算
    if (m_peak_callback) {
        const auto format = get_current_format();
        if (format.encoding != AudioEncoding::INVALID) {
            const float peak_value = get_volume_peak(audio_buffer, format);
            m_peak_callback(peak_value);
        }
    }
}

float audio_manager_impl_linux::get_volume_peak(std::span<const std::byte> audio_buffer, const AudioFormat& format) const
{
    constexpr size_t MAX_SAMPLES = 100; // 最多处理100个采样点
    const size_t sample_size = format.bit_depth / 8;
    const size_t total_samples = audio_buffer.size() / sample_size;
    const size_t step = (std::max<size_t>)(1, total_samples / MAX_SAMPLES);

    float max_peak = 0.0f;
    size_t processed_samples = 0;

    switch (format.encoding) {
    case AudioEncoding::PCM_F32LE: {
        auto* data = reinterpret_cast<const float*>(audio_buffer.data());
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            max_peak = (std::max)(max_peak, std::abs(data[i]));
            if (++processed_samples >= MAX_SAMPLES)
                break;
        }
        break;
    }
    case AudioEncoding::PCM_S16LE: {
        auto* data = reinterpret_cast<const int16_t*>(audio_buffer.data());
        constexpr float scale = 1.0f / 32768.0f;
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            max_peak = (std::max)(max_peak, std::abs(data[i] * scale));
            if (++processed_samples >= MAX_SAMPLES)
                break;
        }
        break;
    }
    case AudioEncoding::PCM_S24LE: {
        const auto* data = reinterpret_cast<const uint8_t*>(audio_buffer.data());
        constexpr float scale = 1.0f / 8388608.0f;
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            // 24-bit小端处理优化：直接内存访问
            int32_t sample = static_cast<int32_t>(data[0] << 8 | data[1] << 16 | data[2] << 24);
            sample >>= 8; // 符号扩展
            max_peak = (std::max)(max_peak, std::abs(sample * scale));
            data += 3 * step * format.channels;
            if (++processed_samples >= MAX_SAMPLES)
                break;
        }
        break;
    }
    case AudioEncoding::PCM_S32LE: {
        auto* data = reinterpret_cast<const int32_t*>(audio_buffer.data());
        constexpr float scale = 1.0f / 2147483648.0f;
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            max_peak = (std::max)(max_peak, std::abs(data[i] * scale));
            if (++processed_samples >= MAX_SAMPLES)
                break;
        }
        break;
    }
    case AudioEncoding::PCM_U8: {
        auto* data = reinterpret_cast<const uint8_t*>(audio_buffer.data());
        constexpr float scale = 1.0f / 128.0f;
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            max_peak = (std::max)(max_peak, std::abs((data[i] - 128) * scale));
            if (++processed_samples >= MAX_SAMPLES)
                break;
        }
        break;
    }
    default:
        return 0.0f;
    }

    return max_peak;
}

// PipeWire格式转换为AudioEncoding
inline audio_manager::AudioEncoding audio_manager_impl_linux::get_AudioEncoding_from_spa_format(spa_audio_format format)
{
    switch (format) {
    case SPA_AUDIO_FORMAT_F32_LE:
        return AudioEncoding::PCM_F32LE;
    case SPA_AUDIO_FORMAT_S16_LE:
        return AudioEncoding::PCM_S16LE;
    case SPA_AUDIO_FORMAT_S24_LE:
        return AudioEncoding::PCM_S24LE;
    case SPA_AUDIO_FORMAT_S32_LE:
        return AudioEncoding::PCM_S32LE;
    case SPA_AUDIO_FORMAT_U8:
        return AudioEncoding::PCM_U8;
    default:
        return AudioEncoding::INVALID;
    }
}

inline spa_audio_format audio_manager_impl_linux::convert_AudioEncoding_to_spa_audio_format(AudioEncoding encoding)
{
    switch (encoding) {
    case AudioEncoding::PCM_S16LE:
        return SPA_AUDIO_FORMAT_S16_LE;
    case AudioEncoding::PCM_S24LE:
        return SPA_AUDIO_FORMAT_S24_32_LE; // 24-bit packed in 32-bit
    case AudioEncoding::PCM_S32LE:
        return SPA_AUDIO_FORMAT_S32_LE;
    case AudioEncoding::PCM_F32LE:
        return SPA_AUDIO_FORMAT_F32_LE;
    case AudioEncoding::PCM_U8:
        return SPA_AUDIO_FORMAT_U8;
    default:
        throw std::invalid_argument("Unsupported audio encoding");
    }
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
    auto* mgr = static_cast<audio_manager_impl_linux*>(userdata);
    if (!mgr || !mgr->p_stream) {
        spdlog::warn("[Linux] Invalid manager or stream in callback.");
        return;
    }

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
    mgr->process_audio_buffer(std::span<const std::byte>(data, n_bytes));

    pw_stream_queue_buffer(mgr->p_stream, buffer);
}

// 获取首选音频格式
audio_manager::AudioFormat audio_manager_impl_linux::get_current_format() const
{
    return m_stream_config;
}

audio_manager::AudioFormat audio_manager_impl_linux::get_preferred_format() const
{
    // TODO: should return PipeWire recommend stream format
    return AudioFormat(AudioEncoding::PCM_F32LE, 2, 48000);
}

#endif // __linux__