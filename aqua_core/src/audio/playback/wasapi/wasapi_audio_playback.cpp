#include "audio/playback/wasapi/wasapi_audio_playback.h"

#include "../../public/audio_fill_silence.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/logger/logger.h"
#include "audio/public/wasapi/wasapi_com.h"
#include "audio/public/wasapi/wasapi_error.h"
#include "audio/public/wasapi/wasapi_format.h"
#include "audio/public/wasapi/wasapi_lifecycle.h"
#include "audio/public/wasapi/wasapi_string.h"

// clang-format off: windows.h 必须先于 avrt.h 等 SDK 头（avrt.h 不自带
// windows.h）；顺序 load-bearing，禁止排序。
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
// clang-format on

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

// RT（音频渲染线程）诊断日志开关：默认关闭。下面的 log 位于 MMCSS Pro Audio
// 的渲染线程内，spdlog 内部有锁，同步调用会破坏实时契约；仅离线排查时临时置 1。
// 边界：只覆盖"流已启动后"的渲染循环 + 线程进出标记。流建立前的设备打开 /
// 格式协商失败日志不设门——此时 start() 仍阻塞在 start_state 上、流尚未运行，
// 不存在音频故障风险，而它们正是"设备打不开"的唯一诊断。
#ifndef AQUA_CLIENT_RT_DEBUG_LOG
#define AQUA_CLIENT_RT_DEBUG_LOG 0
#endif

namespace aqua::audio::wasapi {
namespace {

    [[nodiscard]] UINT32 choose_period(
        UINT32 requested,
        UINT32 default_period,
        UINT32 fundamental,
        UINT32 minimum,
        UINT32 maximum) noexcept
    {
        if (fundamental == 0 || minimum == 0 || maximum < minimum) {
            return 0;
        }

        const UINT32 min_multiple = (minimum + fundamental - 1U) / fundamental;
        const UINT32 max_multiple = maximum / fundamental;
        if (min_multiple > max_multiple) {
            return 0;
        }

        if (requested == 0) {
            const UINT32 clamped_default = std::clamp(default_period, minimum, maximum);
            return (clamped_default / fundamental) * fundamental;
        }

        UINT64 requested_multiple = (static_cast<UINT64>(requested) + fundamental / 2U) / fundamental;
        requested_multiple = std::clamp<UINT64>(
            requested_multiple, min_multiple, max_multiple);

        return static_cast<UINT32>(requested_multiple * fundamental);
    }

} // namespace

WasapiAudioPlayback::WasapiAudioPlayback(AudioDeviceManager& device_manager)
    : device_manager_(device_manager)
{
    log_debug("WASAPI playback backend instance created");
}

WasapiAudioPlayback::~WasapiAudioPlayback()
{
    stop();
}

std::expected<void, AudioError> WasapiAudioPlayback::start(
    const AudioPlaybackConfig& config,
    AudioPlaybackCallback callback,
    AudioPlaybackEventCallback event_callback) noexcept
{
    log_debug_fmt("WASAPI playback config: device={} format={}ch/{}Hz/enc={} buffer_frames={}",
        config.device ? config.device->value() : std::string("default"),
        config.format.channels, config.format.sample_rate, static_cast<int>(config.format.encoding),
        config.frames_per_buffer);

    if (running_.load(std::memory_order_acquire) || audio_thread_.joinable() || event_thread_.joinable()) {
        log_error("WASAPI playback: start rejected because playback is already running");
        return std::unexpected(AudioError::AlreadyRunning);
    }

    if (!callback) {
        log_error("WASAPI playback: start rejected because callback is empty");
        return std::unexpected(AudioError::InvalidArgument);
    }
    if (!config.format.is_valid()) {
        log_error("WASAPI playback: start rejected because requested format is invalid");
        return std::unexpected(AudioError::InvalidArgument);
    }

    const auto resolved = device_manager_.resolve(AudioDeviceDirection::OUTPUT, config.device);
    if (!resolved) {
        log_error_fmt("WASAPI playback: device resolution failed: {}", audio_error_name(resolved.error()));
        return std::unexpected(resolved.error());
    }
    log_debug_fmt("WASAPI playback device resolved: id='{}' name='{}' default={}",
        resolved->id.value(), resolved->name, resolved->is_default);

    ScopedHandle stop_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ScopedHandle audio_event(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    ScopedHandle error_event(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!stop_event || !audio_event || !error_event) {
        const auto error = ::GetLastError();
        log_error_fmt("WASAPI playback: failed to create synchronization events (code={} message={})",
            error, format_system_error_message(std::error_code(static_cast<int>(error), std::system_category())));
        return std::unexpected(AudioError::BackendFailed);
    }

    stop_event_ = stop_event.release();
    audio_event_ = audio_event.release();
    error_event_ = error_event.release();

    frame_callback_ = std::move(callback);
    event_callback_ = std::move(event_callback);
    pending_error_.store(AudioError::None, std::memory_order_release);

    const auto start_state = std::make_shared<StreamStartState>();
    try {
        audio_thread_ = std::jthread(
            [this, device_id = resolved->id.value(), config, start_state](std::stop_token st) {
                // request_stop（stop()/析构）→ SetEvent 唤醒 WaitForMultipleObjects，
                // 否则 jthread 析构的 auto-join 会挂在事件等待上。句柄在 join 之后
                // 才关闭（见 stop()），存活的 stop_callback 不会看到已关闭句柄。
                const std::stop_callback wake_on_stop(st, [this] {
                    if (stop_event_ != nullptr) {
                        ::SetEvent(static_cast<HANDLE>(stop_event_));
                    }
                });
                audio_thread_main(std::move(device_id), config, start_state);
            });
    } catch (...) {
        stop();
        return std::unexpected(AudioError::BackendFailed);
    }

    {
        std::unique_lock lock(start_state->mutex);
        start_state->cv.wait(lock, [&] { return start_state->completed; });
    }

    if (start_state->result != AudioError::None) {
        log_debug_fmt("WASAPI playback initialization failed before event thread start: {}", audio_error_name(start_state->result));
        stop();
        return std::unexpected(start_state->result);
    }

    try {
        event_thread_ = std::jthread([this](std::stop_token st) {
            const std::stop_callback wake_on_stop(st, [this] {
                if (error_event_ != nullptr) {
                    ::SetEvent(static_cast<HANDLE>(error_event_));
                }
            });
            event_thread_main();
        });
        log_debug("WASAPI playback error-event thread started");
    } catch (const std::system_error& e) {
        log_error_fmt("WASAPI playback: failed to start error event thread: code={} message={}",
            e.code().value(), format_system_error_message(e.code()));
        stop();
        return std::unexpected(AudioError::BackendFailed);
    } catch (const std::exception& e) {
        log_error_fmt("WASAPI playback: failed to start error event thread: {}", format_exception_message(e));
        stop();
        return std::unexpected(AudioError::BackendFailed);
    } catch (...) {
        log_error("WASAPI playback: failed to start error event thread");
        stop();
        return std::unexpected(AudioError::BackendFailed);
    }

    log_info_fmt("WASAPI playback started: device={} performance={} frames_per_burst={} capacity={} format={}ch/{}Hz",
        resolved->id.value(),
        audio_stream_performance_name(info_performance_mode_.load(std::memory_order_relaxed)),
        info_frames_per_burst_.load(std::memory_order_relaxed),
        info_buffer_frames_.load(std::memory_order_relaxed),
        config.format.channels, config.format.sample_rate);
    return { };
}

bool WasapiAudioPlayback::is_running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

AudioStreamInfo WasapiAudioPlayback::stream_info() const noexcept
{
    // sample_rate=0 表示尚未 start（或已 stop 清零）→ backend=None。
    if (info_sample_rate_.load(std::memory_order_relaxed) == 0) {
        return { };
    }
    AudioStreamInfo info;
    info.backend = AudioStreamInfo::Backend::Wasapi;
    info.sample_rate = info_sample_rate_.load(std::memory_order_relaxed);
    info.channels = info_channels_.load(std::memory_order_relaxed);
    info.performance_mode = info_performance_mode_.load(std::memory_order_relaxed);
    info.frames_per_burst = info_frames_per_burst_.load(std::memory_order_relaxed);
    // shared mode 事件缓冲：端点缓冲即容量（策略 = 永远填满设备缓冲）。
    info.buffer_capacity_frames = info_buffer_frames_.load(std::memory_order_relaxed);
    // 运行期统计（Gauge）：实际渲染趟数 + 端点缓冲当前填充（GetCurrentPadding 缓存）。
    info.callback_count = stats_callback_count_.load(std::memory_order_relaxed);
    info.current_padding_frames = info_padding_frames_.load(std::memory_order_relaxed);
    // 激活的 endpoint 即所请求设备（playback_switching_design.md §8）。
    try {
        std::lock_guard lock(info_device_mutex_);
        info.device_id = info_device_id_;
    } catch (...) {
        // lock 失败理论上不可达；device_id 留空即可（noexcept 契约优先）。
    }
    return info;
}

void WasapiAudioPlayback::stop() noexcept
{
    log_debug("WASAPI playback stop requested");
    // 先同时置位两个事件：无论 stop() 从哪个线程调用，audio/event 两个工作
    // 线程都会被唤醒并退出（自连接场景下当前线程在返回后由自身循环体退出）。
    // jthread：request_stop 同步执行线程体内注册的 stop_callback（SetEvent 唤醒
    // WaitForMultipleObjects），与原先直接 SetEvent 等价；线程未启动（start
    // 失败路径）时为空操作。两个线程各自唤醒。
    audio_thread_.request_stop();
    event_thread_.request_stop();

    // join 不能自连接（会抛 std::system_error）。自连接时跳过对自身的 join，
    // 但仍 join 另一工作线程；handle 与回调的回收推迟到后续 stop()/析构完成，
    // 避免在本线程仍在使用这些 handle 时提前关闭。
    const bool on_audio_thread = audio_thread_.joinable()
        && audio_thread_.get_id() == std::this_thread::get_id();
    const bool on_event_thread = event_thread_.joinable()
        && event_thread_.get_id() == std::this_thread::get_id();

    if (audio_thread_.joinable() && !on_audio_thread) {
        audio_thread_.join();
    }
    if (event_thread_.joinable() && !on_event_thread) {
        event_thread_.join();
    }

    if (on_audio_thread || on_event_thread) {
        return;
    }

    frame_callback_ = nullptr;
    event_callback_ = nullptr;
    pending_error_.store(AudioError::None, std::memory_order_release);
    running_.store(false, std::memory_order_release);

    // 诊断缓存清零（线程已 join，无并发写）：stream_info() 回到 backend=None。
    info_sample_rate_.store(0, std::memory_order_relaxed);
    info_channels_.store(0, std::memory_order_relaxed);
    info_performance_mode_.store(0, std::memory_order_relaxed);
    info_frames_per_burst_.store(0, std::memory_order_relaxed);
    info_buffer_frames_.store(0, std::memory_order_relaxed);
    stats_callback_count_.store(0, std::memory_order_relaxed);
    info_padding_frames_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock(info_device_mutex_);
        info_device_id_ = AudioDeviceId { };
    }

    if (stop_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
    }
    if (audio_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(audio_event_));
        audio_event_ = nullptr;
    }
    if (error_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(error_event_));
        error_event_ = nullptr;
    }
}

void WasapiAudioPlayback::audio_thread_main(
    std::string device_id,
    AudioPlaybackConfig config,
    std::shared_ptr<StreamStartState> start_state) noexcept
{
    try {
        audio_thread_main_impl(std::move(device_id), std::move(config), start_state);
    } catch (const std::exception& e) {
        log_error_fmt("WASAPI playback audio thread exception: {}", format_exception_message(e));
        bool startup_pending = false;
        {
            std::lock_guard lock(start_state->mutex);
            startup_pending = !start_state->completed;
        }
        if (startup_pending) {
            signal_start_state(start_state, AudioError::BackendFailed);
        } else {
            running_.store(false, std::memory_order_release);
            pending_error_.store(AudioError::BackendFailed, std::memory_order_release);
            if (error_event_ != nullptr) {
                ::SetEvent(static_cast<HANDLE>(error_event_));
            }
        }
    } catch (...) {
        log_error("WASAPI playback audio thread exception: unknown exception");
        bool startup_pending = false;
        {
            std::lock_guard lock(start_state->mutex);
            startup_pending = !start_state->completed;
        }
        if (startup_pending) {
            signal_start_state(start_state, AudioError::BackendFailed);
        } else {
            running_.store(false, std::memory_order_release);
            pending_error_.store(AudioError::BackendFailed, std::memory_order_release);
            if (error_event_ != nullptr) {
                ::SetEvent(static_cast<HANDLE>(error_event_));
            }
        }
    }
}

void WasapiAudioPlayback::audio_thread_main_impl(
    std::string device_id,
    AudioPlaybackConfig config,
    std::shared_ptr<StreamStartState> start_state)
{
    ScopedComInitialization com;
    if (!com.usable()) {
        signal_start_state(start_state, AudioError::BackendFailed);
        return;
    }

    ScopedMmcssTask mmcss;

    IMMDeviceEnumerator* raw_enumerator = nullptr;
    HRESULT hr = ::CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&raw_enumerator));
    if (FAILED(hr) || raw_enumerator == nullptr) {
        log_error_fmt("WASAPI playback: CoCreateInstance(MMDeviceEnumerator) failed: {}",
            hresult_hex(hr));
        signal_start_state(start_state, map_start_hresult(hr));
        return;
    }
    ComPtr<IMMDeviceEnumerator> enumerator(raw_enumerator);

    std::wstring wide_id = wasapi::wide_from_utf8(device_id);
    if (wide_id.empty()) {
        signal_start_state(start_state, AudioError::InvalidArgument);
        return;
    }

    IMMDevice* raw_device = nullptr;
    hr = enumerator->GetDevice(wide_id.c_str(), &raw_device);
    if (FAILED(hr) || raw_device == nullptr) {
        signal_start_state(start_state, AudioError::DeviceNotFound);
        return;
    }
    ComPtr<IMMDevice> device(raw_device);

    IAudioClient* raw_audio_client = nullptr;
    hr = device->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(&raw_audio_client));
    if (FAILED(hr) || raw_audio_client == nullptr) {
        log_error_fmt("WASAPI playback: Activate(IAudioClient) failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_start_hresult(hr));
        return;
    }
    ComPtr<IAudioClient> audio_client(raw_audio_client);

    WAVEFORMATEX* raw_mix_format = nullptr;
    hr = audio_client->GetMixFormat(&raw_mix_format);
    if (FAILED(hr) || raw_mix_format == nullptr) {
        log_error_fmt("WASAPI playback: GetMixFormat failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_start_hresult(hr));
        return;
    }
    // CoTaskPtr 接管：下面的日志会分配（可抛 bad_alloc），裸 CoTaskMemFree 会被跳过。
    // （capture 侧早已是 unique_ptr 风格，此处对齐；对象 8 字节、无函数指针开销。）
    const CoTaskPtr<WAVEFORMATEX> mix_format(raw_mix_format);
    log_debug_fmt("WASAPI playback device mix format: tag={} channels={} rate={} bits={} block_align={}",
        mix_format->wFormatTag, mix_format->nChannels, mix_format->nSamplesPerSec,
        mix_format->wBitsPerSample, mix_format->nBlockAlign);

    auto requested_wave = make_wave_format(config.format);
    if (!requested_wave) {
        signal_start_state(start_state, AudioError::InvalidArgument);
        return;
    }
    const WAVEFORMATEX* stream_format = requested_wave->get();
    log_debug_fmt("WASAPI playback stream format requested: block_align={} samples_per_sec={} bits_per_sample={}",
        stream_format->nBlockAlign, stream_format->nSamplesPerSec, stream_format->wBitsPerSample);

    ComPtr<IAudioClient3> audio_client3;
    {
        IAudioClient3* raw_client3 = nullptr;
        if (SUCCEEDED(audio_client->QueryInterface(
                __uuidof(IAudioClient3), reinterpret_cast<void**>(&raw_client3)))
            && raw_client3 != nullptr) {
            audio_client3.reset(raw_client3);
        }
    }

    bool use_client3 = false;
    UINT32 period_frames = 0;
    UINT32 fundamental_period = 0; // 引擎基本周期（仅 IAudioClient3 可知；诊断用）
    log_debug_fmt("WASAPI playback: IAudioClient3 {}", audio_client3 ? "available" : "unavailable");
    if (audio_client3) {
        AudioClientProperties properties { };
        properties.cbSize = sizeof(properties);
        properties.bIsOffload = FALSE;
        properties.eCategory = AudioCategory_Media;
        properties.Options = AUDCLNT_STREAMOPTIONS_MATCH_FORMAT;

        const HRESULT properties_hr = audio_client3->SetClientProperties(&properties);
        if (SUCCEEDED(properties_hr)) {
            WAVEFORMATEX* raw_closest = nullptr;
            const HRESULT support_hr = audio_client3->IsFormatSupported(
                AUDCLNT_SHAREMODE_SHARED,
                stream_format,
                &raw_closest);
            const CoTaskPtr<WAVEFORMATEX> closest_match { raw_closest };
            if (support_hr == S_OK) {
                UINT32 default_period = 0;
                UINT32 minimum_period = 0;
                UINT32 maximum_period = 0;

                const HRESULT period_hr = audio_client3->GetSharedModeEnginePeriod(
                    stream_format,
                    &default_period,
                    &fundamental_period,
                    &minimum_period,
                    &maximum_period);
                if (SUCCEEDED(period_hr)) {
                    period_frames = choose_period(
                        config.frames_per_buffer,
                        default_period,
                        fundamental_period,
                        minimum_period,
                        maximum_period);
                    if (period_frames != 0) {
                        use_client3 = true;
                        log_debug_fmt(
                            "WASAPI playback: IAudioClient3 period default={} fundamental={} min={} max={} selected={}",
                            default_period,
                            fundamental_period,
                            minimum_period,
                            maximum_period,
                            period_frames);
                    }
                }
            } else if (support_hr != AUDCLNT_E_UNSUPPORTED_FORMAT) {
                log_warn_fmt("WASAPI playback: IAudioClient3 IsFormatSupported failed: {}",
                    hresult_hex(support_hr));
            }
        } else {
            log_warn_fmt("WASAPI playback: IAudioClient3::SetClientProperties failed: {}",
                hresult_hex(properties_hr));
        }
    }

    if (!use_client3) {
        WAVEFORMATEX* raw_closest = nullptr;
        const HRESULT support_hr = audio_client->IsFormatSupported(
            AUDCLNT_SHAREMODE_SHARED,
            stream_format,
            &raw_closest);
        const CoTaskPtr<WAVEFORMATEX> closest_match { raw_closest };
        // S_OK = 原生支持；S_FALSE = 引擎可重采样（closest_match 即 mix format，
        // 共享模式直接沿用请求格式、由引擎转换）。仅 FAILED 才是真正不支持。
        if (FAILED(support_hr)) {
            log_error_fmt("WASAPI playback: requested format unsupported: {}", hresult_hex(support_hr));
            signal_start_state(start_state,
                support_hr == AUDCLNT_E_UNSUPPORTED_FORMAT
                    ? AudioError::FormatUnsupported
                    : map_start_hresult(support_hr));
            return;
        }
    }

    const DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (use_client3) {
        hr = audio_client3->InitializeSharedAudioStream(
            stream_flags,
            period_frames,
            stream_format,
            nullptr);

        // 优先用 IAudioClient3，但设备/引擎可能拒绝所请求的低延迟周期，
        // 即便旧的 shared-mode 路径可用。此时回退到 IAudioClient。
        if (FAILED(hr)) {
            log_warn_fmt(
                "WASAPI playback: IAudioClient3::InitializeSharedAudioStream failed: {}; falling back to IAudioClient",
                hresult_hex(hr));
            use_client3 = false;

            WAVEFORMATEX* raw_closest = nullptr;
            const HRESULT support_hr = audio_client->IsFormatSupported(
                AUDCLNT_SHAREMODE_SHARED,
                stream_format,
                &raw_closest);
            const CoTaskPtr<WAVEFORMATEX> closest_match { raw_closest };
            // 同前：S_FALSE（引擎可重采样）非错误，仅 FAILED 才拒绝。
            if (FAILED(support_hr)) {
                signal_start_state(start_state,
                    support_hr == AUDCLNT_E_UNSUPPORTED_FORMAT
                        ? AudioError::FormatUnsupported
                        : map_start_hresult(support_hr));
                return;
            }
        }
    }
    if (!use_client3) {
        // 在 shared-mode 事件驱动缓冲下，两个 duration 参数都必须为 0；
        // 由 WASAPI 依据音频引擎调度周期自行决定缓冲大小。
        hr = audio_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            stream_flags,
            0,
            0,
            stream_format,
            nullptr);
    }
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: initialize shared stream failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_start_hresult(hr));
        return;
    }

    hr = audio_client->SetEventHandle(static_cast<HANDLE>(audio_event_));
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: SetEventHandle failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_start_hresult(hr));
        return;
    }

    UINT32 buffer_frames = 0;
    hr = audio_client->GetBufferSize(&buffer_frames);
    if (FAILED(hr) || buffer_frames == 0) {
        log_error_fmt("WASAPI playback: GetBufferSize failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_start_hresult(hr));
        return;
    }
    log_debug_fmt("WASAPI playback stream ready: device={} mode={} period_frames={} buffer_frames={} block_align={} rate={} channels={} bits={}",
        device_id, use_client3 ? "IAudioClient3" : "IAudioClient", period_frames, buffer_frames,
        stream_format->nBlockAlign, stream_format->nSamplesPerSec, stream_format->nChannels,
        stream_format->wBitsPerSample);

    // ---- 缓存实际 stream 运行参数（诊断 stream_info()；一次性快照）----
    info_sample_rate_.store(stream_format->nSamplesPerSec, std::memory_order_relaxed);
    info_channels_.store(stream_format->nChannels, std::memory_order_relaxed);
    info_performance_mode_.store(
        use_client3 ? AudioStreamInfo::kPerformanceLowLatency : AudioStreamInfo::kPerformanceNone,
        std::memory_order_relaxed);
    info_frames_per_burst_.store(fundamental_period, std::memory_order_relaxed);
    info_buffer_frames_.store(buffer_frames, std::memory_order_relaxed);
    {
        std::lock_guard lock(info_device_mutex_);
        info_device_id_ = AudioDeviceId(device_id);
    }

    IAudioRenderClient* raw_render_client = nullptr;
    hr = audio_client->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(&raw_render_client));
    if (FAILED(hr) || raw_render_client == nullptr) {
        log_error_fmt("WASAPI playback: GetService(IAudioRenderClient) failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_start_hresult(hr));
        return;
    }
    ComPtr<IAudioRenderClient> render_client(raw_render_client);

    // Start 之前先用静音预填充 endpoint。这样能在 start() 返回前
    // 避免确定性的启动 underrun，且不会提前触发应用回调。
    // 静音字节规则的唯一来源是 AudioFormat::silence_byte()（U8=0x80，其余=0x00）。
    const std::byte silence_fill = config.format.silence_byte();
    BYTE* data = nullptr;
    hr = render_client->GetBuffer(buffer_frames, &data);
    if (FAILED(hr) || data == nullptr) {
        log_error_fmt("WASAPI playback: initial GetBuffer failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_start_hresult(hr));
        return;
    }
    std::ranges::fill(
        std::span { reinterpret_cast<std::byte*>(data),
            static_cast<std::size_t>(buffer_frames) * stream_format->nBlockAlign },
        silence_fill);
    hr = render_client->ReleaseBuffer(buffer_frames, 0);
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: initial ReleaseBuffer failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_start_hresult(hr));
        return;
    }

    hr = audio_client->Start();
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: Start failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_start_hresult(hr));
        return;
    }

    log_debug_fmt("WASAPI playback starting stream: device={} format={}ch/{}Hz mode={} period_frames={} buffer_frames={}",
        device_id, config.format.channels, config.format.sample_rate,
        use_client3 ? "IAudioClient3" : "IAudioClient", period_frames, buffer_frames);
    running_.store(true, std::memory_order_release);
    signal_start_state(start_state, AudioError::None);

    HANDLE wait_handles[2] {
        static_cast<HANDLE>(stop_event_),
        static_cast<HANDLE>(audio_event_),
    };

    // 运行期失败退出：必须**先**置 running_=false 再 SetEvent。
    // 反序会让 event 线程在 running_ 仍为 true 时派发错误回调；上层
    // (ClientRuntime::service_playback_recovery) 据此认定"当前流健康"，把真实
    // 故障当成"被替换旧流的迟到错误"吸收掉，从而跳过本次恢复（只能等 500ms
    // 后的 silent-death 兜底，且错误通道会先误报"已恢复"）。
    auto fail_with = [this](AudioError e) noexcept {
        running_.store(false, std::memory_order_release);
        pending_error_.store(e, std::memory_order_release);
        ::SetEvent(static_cast<HANDLE>(error_event_));
    };

    for (;;) {
        const DWORD wait_result = ::WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0) {
            // RT 线程日志（见本文件顶部 AQUA_CLIENT_RT_DEBUG_LOG）。
#if AQUA_CLIENT_RT_DEBUG_LOG
            log_debug("WASAPI playback stop event received");
#endif
            break;
        }
        if (wait_result != WAIT_OBJECT_0 + 1) {
            fail_with(AudioError::BackendFailed);
            break;
        }

        UINT32 padding_frames = 0;
        hr = audio_client->GetCurrentPadding(&padding_frames);
        if (FAILED(hr)) {
            fail_with(map_runtime_hresult(hr));
            break;
        }
        if (padding_frames > buffer_frames) {
            fail_with(AudioError::BackendFailed);
            break;
        }
        // 运行期统计：缓存当前 padding（音频线程写，诊断线程只读缓存，
        // 绝不跨线程调 GetCurrentPadding）。
        info_padding_frames_.store(padding_frames, std::memory_order_relaxed);

        const UINT32 available_frames = buffer_frames - padding_frames;
        if (available_frames == 0) {
            continue;
        }
        // 实际渲染趟数（真正会调用 frame_callback_ 的次数）。
        stats_callback_count_.fetch_add(1, std::memory_order_relaxed);

        data = nullptr;
        hr = render_client->GetBuffer(available_frames, &data);
        if (FAILED(hr) || data == nullptr) {
            fail_with(map_runtime_hresult(hr));
            break;
        }

        const std::size_t output_bytes = static_cast<std::size_t>(available_frames) * stream_format->nBlockAlign;
        std::span<std::byte> output(
            reinterpret_cast<std::byte*>(data), output_bytes);

        std::uint32_t written_frames = 0;
        try {
            written_frames = frame_callback_(output);
        } catch (const std::exception& e) {
#if AQUA_CLIENT_RT_DEBUG_LOG
            log_error_fmt("WASAPI playback callback exception: {}", format_exception_message(e));
#else
            (void)e;
#endif
            written_frames = 0;
            pending_error_.store(AudioError::BackendFailed, std::memory_order_release);
        } catch (...) {
#if AQUA_CLIENT_RT_DEBUG_LOG
            log_error("WASAPI playback callback exception: unknown exception");
#endif
            written_frames = 0;
            pending_error_.store(AudioError::BackendFailed, std::memory_order_release);
        }

        if (written_frames > available_frames) {
            // RT 线程日志（见本文件顶部 AQUA_CLIENT_RT_DEBUG_LOG）。
#if AQUA_CLIENT_RT_DEBUG_LOG
            log_error_fmt(
                "WASAPI playback callback returned {} frames, but only {} are available",
                written_frames,
                available_frames);
#endif
            written_frames = 0;
            pending_error_.store(AudioError::InvalidArgument, std::memory_order_release);
        }

        fill_silence_tail(output,
            static_cast<std::size_t>(written_frames) * stream_format->nBlockAlign,
            silence_fill);

        hr = render_client->ReleaseBuffer(available_frames, 0);
        if (FAILED(hr)) {
            fail_with(map_runtime_hresult(hr));
            break;
        }

        if (pending_error_.load(std::memory_order_acquire) != AudioError::None) {
            running_.store(false, std::memory_order_release);
            ::SetEvent(static_cast<HANDLE>(error_event_));
            break;
        }
    }

    (void)audio_client->Stop();
    running_.store(false, std::memory_order_release);
    // RT 线程日志（见本文件顶部 AQUA_CLIENT_RT_DEBUG_LOG）。
#if AQUA_CLIENT_RT_DEBUG_LOG
    log_debug("WASAPI playback audio thread exited");
#endif
}

void WasapiAudioPlayback::event_thread_main() noexcept
{
    log_debug("WASAPI playback error-event thread entered");
    const HANDLE wait_handles[2] {
        static_cast<HANDLE>(stop_event_),
        static_cast<HANDLE>(error_event_),
    };

    for (;;) {
        const DWORD result = ::WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            log_debug("WASAPI playback error-event thread stopped by shutdown");
            return;
        }
        if (result != WAIT_OBJECT_0 + 1) {
            log_error_fmt("WASAPI playback error-event thread wait failed: result={}", result);
            return;
        }

        const AudioError error = pending_error_.exchange(
            AudioError::None, std::memory_order_acq_rel);
        if (error == AudioError::None) {
            continue;
        }
        log_debug_fmt("WASAPI playback error event received: {}", audio_error_name(error));
        if (event_callback_) {
            try {
                event_callback_(error);
            } catch (...) {
                log_error("WASAPI playback event callback exception");
            }
        } else {
            log_warn_fmt("WASAPI playback runtime error: {}", audio::audio_error_name(error));
        }
        return;
    }
}

} // namespace aqua::audio::wasapi
