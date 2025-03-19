//
// Created by aquawius on 25-1-31.
//

#include "audio_playback_linux.h"
#include "version.h"

#include <boost/endian/conversion.hpp>
#include <spdlog/spdlog.h>

#ifdef __linux__
#include <spa/utils/result.h>

// PipeWire 回调声明
void on_playback_process(void* userdata);
void on_stream_state_changed_cb(void* userdata, pw_stream_state old, pw_stream_state state, const char* error);

static constexpr struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed_cb,
    .process = on_playback_process,
};

audio_playback_linux::audio_playback_linux()
{
    spdlog::debug("[Linux] Audio playback instance created.");
}

audio_playback_linux::~audio_playback_linux()
{
    stop_playback(); // 确保先停止播放

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
    spdlog::info("[Linux] Audio playback destroyed.");
}

// 初始化
bool audio_playback_linux::init()
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
bool audio_playback_linux::setup_stream()
{
    if (!p_main_loop || !p_context) {
        spdlog::error("[Linux] setup_stream() failed: PipeWire not initialized.");
        return false;
    }

    // 配置流参数
    const std::string latency_str = std::to_string(m_pw_stream_latency) + "/" + std::to_string(m_stream_config.sample_rate);
    const std::string stream_name = std::string(aqua_client_BINARY_NAME) + " playback";

    // 创建流属性
    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        // PW_KEY_STREAM_PLAYBACK_SOURCE, "true",   // 对于播放，这个属性不需要
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
        PW_DIRECTION_OUTPUT, // 播放
        PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_RT_PROCESS),
        p_params,
        1);

    if (ret < 0) {
        spdlog::error("[Linux] Stream connection failed: {} (code: {})", spa_strerror(ret), ret);
        return false;
    }

    spdlog::info("[Linux] Stream configured for playback: {} Hz, {} channels",
        m_stream_config.sample_rate, m_stream_config.channels);
    return true;
}

// 播放控制
bool audio_playback_linux::start_playback()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_is_playing) {
        spdlog::warn("[Linux] Playback is already running.");
        return false;
    }

    m_promise_initialized = std::promise<void>();

    // 启动播放线程
    m_playback_thread = std::jthread([this](std::stop_token stop_token) {
        m_is_playing = true;
        m_promise_initialized.set_value();

        // 设置停止回调
        std::stop_callback stop_cb(stop_token, [this]() {
            if (p_main_loop)
                pw_main_loop_quit(p_main_loop);
        });

        // 运行主循环
        spdlog::info("[Linux] Starting PipeWire main loop for playback...");
        pw_main_loop_run(p_main_loop);
        spdlog::info("[Linux] PipeWire main loop exited.");

        m_is_playing = false;
    });

    // 等待线程初始化完成
    m_promise_initialized.get_future().wait();
    return true;
}

bool audio_playback_linux::stop_playback()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_is_playing) {
        spdlog::warn("[Linux] No active playback to stop.");
        return false;
    }

    // 请求线程停止
    m_playback_thread.request_stop();
    if (p_main_loop)
        pw_main_loop_quit(p_main_loop);

    // 等待线程退出
    if (m_playback_thread.joinable()) {
        m_playback_thread.join();
        spdlog::debug("[Linux] Playback thread joined.");
    }

    return true;
}

bool audio_playback_linux::is_playing() const
{
    return m_is_playing;
}

audio_playback::AudioFormat audio_playback_linux::get_current_format() const
{
    return m_stream_config;
}

void audio_playback_linux::set_format(AudioFormat format)
{
    m_stream_config = format;
}

bool audio_playback_linux::push_packet_data(std::span<const std::byte> packet_data)
{
    if (packet_data.empty()) {
        spdlog::warn("[Linux] Empty packet data received");
        return false;
    }
    
    // 将 std::span<const std::byte> 转换为 std::vector<uint8_t>
    std::vector<uint8_t> data_vec(packet_data.size());
    std::memcpy(data_vec.data(), packet_data.data(), packet_data.size());
    
    return m_adaptive_buffer.push_buffer_packets(std::move(data_vec));
}

void audio_playback_linux::set_peak_callback(AudioPeakCallback callback)
{
    m_peak_callback = std::move(callback);
}

// 数据处理
void audio_playback_linux::process_playback_buffer()
{
    struct pw_buffer* b = pw_stream_dequeue_buffer(p_stream);
    if (!b) {
        spdlog::warn("[Linux] Out of buffers");
        return;
    }

    struct spa_buffer* buf = b->buffer;
    float* dst = static_cast<float*>(buf->datas[0].data);
    if (!dst) {
        pw_stream_queue_buffer(p_stream, b);
        return;
    }

    const uint32_t suggested_frames = b->requested;
    const uint32_t max_frames = buf->datas[0].maxsize / sizeof(float) / m_stream_config.channels;
    const uint32_t need_frames = suggested_frames > 0 ? std::min(max_frames, suggested_frames) : max_frames;
    const uint32_t need_bytes = need_frames * m_stream_config.channels * sizeof(float);

    // 创建临时缓冲区接收字节数据
    std::vector<uint8_t> temp_buffer(need_bytes);
    
    // 从缓冲区获取字节数据
    size_t filled_bytes = m_adaptive_buffer.pull_buffer_data(temp_buffer.data(), need_bytes);
    
    // 复制到目标Float缓冲区
    std::memcpy(dst, temp_buffer.data(), filled_bytes);

    process_volume_peak(std::span<const float>(dst, filled_bytes / sizeof(float)));

    if (filled_bytes < need_bytes) {
        spdlog::warn("[audio_playback] Buffer not completely filled: {}/{} bytes",
            filled_bytes, need_bytes);
    }

    const uint32_t filled_frames = filled_bytes / (sizeof(float) * m_stream_config.channels);
    b->size = filled_frames;

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float) * m_stream_config.channels;
    buf->datas[0].chunk->size = filled_bytes;

    pw_stream_queue_buffer(p_stream, b);
}

void audio_playback_linux::process_volume_peak(const std::span<const float> data) const
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

// PipeWire 回调函数
void on_playback_process(void* userdata)
{
    auto* mgr = static_cast<audio_playback_linux*>(userdata);
    if (!mgr || !mgr->p_stream) {
        spdlog::warn("[Linux] Invalid manager or stream in playback callback.");
        return;
    }

    std::lock_guard<std::mutex> lock(mgr->m_mutex);
    if (!mgr->m_is_playing)
        return;

    mgr->process_playback_buffer();
}

void on_stream_state_changed_cb(void* userdata, pw_stream_state old, pw_stream_state state, const char* error)
{
    auto* audio_manager = static_cast<audio_playback_linux*>(userdata);
    if (error)
        spdlog::error("[Linux] Stream error: {}", error);
    spdlog::info("[Linux] Stream state changed: {} -> {}",
        pw_stream_state_as_string(old),
        pw_stream_state_as_string(state));
}

#endif // __linux__