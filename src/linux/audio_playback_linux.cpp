//
// Created by aquawius on 25-1-31.
//

#include "audio_playback_linux.h"
#include "config.h"

#include <boost/endian/conversion.hpp>
#include <spa/utils/result.h>
#include <spdlog/spdlog.h>

#ifdef __linux__

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
    const std::string latency_str = std::to_string(m_stream_config.latency) + "/" + std::to_string(m_stream_config.rate);
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
        .rate = m_stream_config.rate,
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
        m_stream_config.rate, m_stream_config.channels);
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

const audio_playback_linux::stream_config& audio_playback_linux::get_format() const
{
    return m_stream_config;
}

bool audio_playback_linux::push_packet_data(const std::vector<uint8_t>& origin_packet_data)
{
    std::lock_guard<std::mutex> lock(m_packets_buffer_mutex);

    return m_adaptive_buffer.put_buffer_packets(std::vector<uint8_t>(origin_packet_data));
}

inline void display_volume(std::span<const float> data)
{
    if (spdlog::get_level() > spdlog::level::debug || data.empty()) {
        return;
    }

    constexpr size_t METER_WIDTH = 40;
    static std::array<char, METER_WIDTH + 1> meter_buffer;
    meter_buffer.fill('-');

    const size_t size = data.size();

    // 采样几个关键点计算最大值
    float local_peak = std::abs(data[0]); // 起始点
    local_peak = std::max(local_peak, std::abs(data[size - 1])); // 终点
    local_peak = std::max(local_peak, std::abs(data[size / 2])); // 中点
    local_peak = std::max(local_peak, std::abs(data[size / 4])); // 1/4点
    local_peak = std::max(local_peak, std::abs(data[size * 3 / 4])); // 3/4点
    local_peak = std::max(local_peak, std::abs(data[size / 8])); // 1/8点
    local_peak = std::max(local_peak, std::abs(data[size * 7 / 8])); // 7/8点

    // 计算峰值电平并更新音量条
    const int peak_level = std::clamp(static_cast<int>(local_peak * METER_WIDTH), 0,
        static_cast<int>(METER_WIDTH));

    if (peak_level > 0) {
        std::fill_n(meter_buffer.begin(), peak_level, '#');
    }

    meter_buffer[METER_WIDTH] = '\0';
    spdlog::debug("[{}] {:.3f}", meter_buffer.data(), local_peak);
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
    const uint32_t need_samples = need_frames * m_stream_config.channels;

    std::lock_guard<std::mutex> lock(m_packets_buffer_mutex);

    // 直接写入目标缓冲区
    size_t filled_samples = m_adaptive_buffer.get_samples(dst, need_samples);

    if (filled_samples > 0) {
        // 显示音量
        display_volume(std::span<const float>(dst, filled_samples));

        if (filled_samples < need_samples) {
            spdlog::trace("[audio_playback] Buffer not completely filled: {}/{} samples",
                filled_samples, need_samples);
        }
    } else {
        // 即使没有数据，也显示音量（全静音）
        display_volume(std::span<const float>(dst, need_samples));
    }

    const uint32_t filled_frames = filled_samples / m_stream_config.channels;
    b->size = filled_frames;

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float) * m_stream_config.channels;
    buf->datas[0].chunk->size = filled_samples * sizeof(float);

    pw_stream_queue_buffer(p_stream, b);
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
