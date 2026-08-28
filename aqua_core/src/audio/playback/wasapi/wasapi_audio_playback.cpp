#include "audio/playback/wasapi/wasapi_audio_playback.h"

#include "aqua/audio/devices/audio_device_manager.h"
#include "audio/wasapi/wasapi_com.h"
#include "aqua/logger/logger.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <condition_variable>
#include <cstdio>
#include <expected>
#include <memory>
#include <system_error>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace aqua::audio::wasapi {
namespace {

class ScopedHandle final {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    ScopedHandle& operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE handle = nullptr) noexcept
    {
        if (*this) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }

    [[nodiscard]] HANDLE release() noexcept
    {
        return std::exchange(handle_, nullptr);
    }

private:
    HANDLE handle_ = nullptr;
};

class ScopedMmcssTask final {
public:
    ScopedMmcssTask() noexcept
    {
        task_index_ = 0;
        handle_ = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index_);
        if (handle_ == nullptr) {
            const auto error = ::GetLastError();
            log_warn_fmt("WASAPI playback: AvSetMmThreadCharacteristicsW(Pro Audio) failed: code={} message={}",
                error, format_system_error_message(
                    std::error_code(static_cast<int>(error), std::system_category())));
        }
    }

    ~ScopedMmcssTask()
    {
        if (handle_ != nullptr) {
            ::AvRevertMmThreadCharacteristics(handle_);
        }
    }

    ScopedMmcssTask(const ScopedMmcssTask&) = delete;
    ScopedMmcssTask& operator=(const ScopedMmcssTask&) = delete;

private:
    HANDLE handle_ = nullptr;
    DWORD task_index_ = 0;
};

[[nodiscard]] std::string hresult_hex(HRESULT hr)
{
    char buffer[32] {};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(hr));
    return buffer;
}

[[nodiscard]] AudioError map_hresult(HRESULT hr) noexcept
{
    switch (hr) {
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return AudioError::FormatUnsupported;
    case AUDCLNT_E_DEVICE_IN_USE:
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
        return AudioError::DeviceUnavailable;
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case AUDCLNT_E_RESOURCES_INVALIDATED:
        return AudioError::DeviceDisconnected;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return AudioError::BackendFailed;
    case E_ACCESSDENIED:
        return AudioError::PermissionDenied;
    case E_INVALIDARG:
        return AudioError::InvalidArgument;
    default:
        return AudioError::BackendFailed;
    }
}

struct WaveFormatStorage {
    WAVEFORMATEX basic {};
    WAVEFORMATEXTENSIBLE extensible {};
    bool is_extensible = false;

    [[nodiscard]] WAVEFORMATEX* get() noexcept
    {
        return is_extensible ? &extensible.Format : &basic;
    }
};

[[nodiscard]] std::optional<WaveFormatStorage>
make_wave_format(const AudioFormat& format) noexcept
{
    if (!format.is_valid() || format.channels > static_cast<std::uint32_t>(std::numeric_limits<WORD>::max())) {
        return std::nullopt;
    }

    const std::uint32_t frame_bytes = format.frame_bytes();
    const std::uint32_t bits = format.bytes_per_sample() * 8U;
    if (frame_bytes == 0 || bits > static_cast<std::uint32_t>(std::numeric_limits<WORD>::max())) {
        return std::nullopt;
    }

    WaveFormatStorage result;
    const bool use_extensible = format.channels > 2;

    if (!use_extensible) {
        auto& wave = result.basic;
        wave.nChannels = static_cast<WORD>(format.channels);
        wave.nSamplesPerSec = format.sample_rate;
        wave.wBitsPerSample = static_cast<WORD>(bits);
        wave.nBlockAlign = static_cast<WORD>(frame_bytes);
        wave.nAvgBytesPerSec = wave.nSamplesPerSec * wave.nBlockAlign;
        wave.cbSize = 0;

        switch (format.encoding) {
        case AudioEncoding::PCM_F32LE:
            wave.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            break;
        case AudioEncoding::PCM_U8:
        case AudioEncoding::PCM_S16LE:
        case AudioEncoding::PCM_S24LE:
        case AudioEncoding::PCM_S32LE:
            wave.wFormatTag = WAVE_FORMAT_PCM;
            break;
        case AudioEncoding::INVALID:
            return std::nullopt;
        }
        return result;
    }

    auto& wave = result.extensible;
    result.is_extensible = true;
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = static_cast<WORD>(format.channels);
    wave.Format.nSamplesPerSec = format.sample_rate;
    wave.Format.wBitsPerSample = static_cast<WORD>(bits);
    wave.Format.nBlockAlign = static_cast<WORD>(frame_bytes);
    wave.Format.nAvgBytesPerSec = wave.Format.nSamplesPerSec * wave.Format.nBlockAlign;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wave.dwChannelMask = 0;
    wave.Samples.wValidBitsPerSample = wave.Format.wBitsPerSample;

    switch (format.encoding) {
    case AudioEncoding::PCM_F32LE:
        wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        break;
    case AudioEncoding::PCM_U8:
    case AudioEncoding::PCM_S16LE:
    case AudioEncoding::PCM_S24LE:
    case AudioEncoding::PCM_S32LE:
        wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case AudioEncoding::INVALID:
        return std::nullopt;
    }

    return result;
}

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

    UINT64 requested_multiple =
        (static_cast<UINT64>(requested) + fundamental / 2U) / fundamental;
    requested_multiple = std::clamp<UINT64>(
        requested_multiple, min_multiple, max_multiple);

    return static_cast<UINT32>(requested_multiple * fundamental);
}

} // namespace

struct WasapiAudioPlayback::StartState {
    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    AudioError result = AudioError::BackendFailed;
};

void WasapiAudioPlayback::signal_start_state(
    const std::shared_ptr<StartState>& state, AudioError result) noexcept
{
    {
        std::lock_guard lock(state->mutex);
        state->result = result;
        state->completed = true;
    }
    state->cv.notify_one();
}

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

    if (running_.load(std::memory_order_acquire) ||
        audio_thread_.joinable() || event_thread_.joinable()) {
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
            error, format_system_error_message(
                std::error_code(static_cast<int>(error), std::system_category())));
        return std::unexpected(AudioError::BackendFailed);
    }

    stop_event_ = stop_event.release();
    audio_event_ = audio_event.release();
    error_event_ = error_event.release();

    frame_callback_ = std::move(callback);
    event_callback_ = std::move(event_callback);
    pending_error_.store(AudioError::None, std::memory_order_release);

    const auto start_state = std::make_shared<StartState>();
    try {
        audio_thread_ = std::thread(
            &WasapiAudioPlayback::audio_thread_main,
            this,
            resolved->id.value(),
            config,
            start_state);
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
        event_thread_ = std::thread(&WasapiAudioPlayback::event_thread_main, this);
        log_debug("WASAPI playback error-event thread started");
    } catch (const std::system_error& e) {
        log_error_fmt("WASAPI playback: failed to start error event thread: code={} message={}",
            e.code().value(), format_system_error_message(e.code()));
        stop();
        return std::unexpected(AudioError::BackendFailed);
    } catch (const std::exception& e) {
        log_error_fmt("WASAPI playback: failed to start error event thread: {}", e.what());
        stop();
        return std::unexpected(AudioError::BackendFailed);
    } catch (...) {
        log_error("WASAPI playback: failed to start error event thread");
        stop();
        return std::unexpected(AudioError::BackendFailed);
    }

    log_info_fmt("WASAPI playback started: device={} format={}ch/{}Hz",
        resolved->id.value(), config.format.channels, config.format.sample_rate);
    return {};
}

bool WasapiAudioPlayback::is_running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

void WasapiAudioPlayback::stop() noexcept
{
    log_debug("WASAPI playback stop requested");
    if (stop_event_ != nullptr) {
        ::SetEvent(static_cast<HANDLE>(stop_event_));
    }

    if (audio_thread_.joinable()) {
        if (audio_thread_.get_id() == std::this_thread::get_id()) {
            return;
        }
        audio_thread_.join();
    }

    if (error_event_ != nullptr) {
        ::SetEvent(static_cast<HANDLE>(error_event_));
    }

    if (event_thread_.joinable()) {
        if (event_thread_.get_id() == std::this_thread::get_id()) {
            return;
        }
        event_thread_.join();
    }

    frame_callback_ = nullptr;
    event_callback_ = nullptr;
    pending_error_.store(AudioError::None, std::memory_order_release);
    running_.store(false, std::memory_order_release);

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
    std::shared_ptr<StartState> start_state) noexcept
{
    try {
        audio_thread_main_impl(std::move(device_id), std::move(config), start_state);
    } catch (const std::exception& e) {
        log_error_fmt("WASAPI playback audio thread exception: {}", e.what());
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
    std::shared_ptr<StartState> start_state)
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
        signal_start_state(start_state, map_hresult(hr));
        return;
    }
    ComPtr<IMMDeviceEnumerator> enumerator(raw_enumerator);

    int wide_length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        device_id.data(), static_cast<int>(device_id.size()), nullptr, 0);
    if (wide_length <= 0) {
        signal_start_state(start_state, AudioError::InvalidArgument);
        return;
    }
    std::wstring wide_id(static_cast<std::size_t>(wide_length), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS,
            device_id.data(), static_cast<int>(device_id.size()),
            wide_id.data(), wide_length) <= 0) {
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
        signal_start_state(start_state, map_hresult(hr));
        return;
    }
    ComPtr<IAudioClient> audio_client(raw_audio_client);

    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client->GetMixFormat(&mix_format);
    if (FAILED(hr) || mix_format == nullptr) {
        log_error_fmt("WASAPI playback: GetMixFormat failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_hresult(hr));
        return;
    }
    log_debug_fmt("WASAPI playback device mix format: tag={} channels={} rate={} bits={} block_align={}",
        mix_format->wFormatTag, mix_format->nChannels, mix_format->nSamplesPerSec,
        mix_format->wBitsPerSample, mix_format->nBlockAlign);
    ::CoTaskMemFree(mix_format);

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
                __uuidof(IAudioClient3), reinterpret_cast<void**>(&raw_client3))) &&
            raw_client3 != nullptr) {
            audio_client3.reset(raw_client3);
        }
    }

    bool use_client3 = false;
    UINT32 period_frames = 0;
    log_debug_fmt("WASAPI playback: IAudioClient3 {}", audio_client3 ? "available" : "unavailable");
    if (audio_client3) {
        AudioClientProperties properties {};
        properties.cbSize = sizeof(properties);
        properties.bIsOffload = FALSE;
        properties.eCategory = AudioCategory_Media;
        properties.Options = AUDCLNT_STREAMOPTIONS_MATCH_FORMAT;

        const HRESULT properties_hr = audio_client3->SetClientProperties(&properties);
        if (SUCCEEDED(properties_hr)) {
            WAVEFORMATEX* closest_match = nullptr;
            const HRESULT support_hr = audio_client3->IsFormatSupported(
                AUDCLNT_SHAREMODE_SHARED,
                stream_format,
                &closest_match);
            if (closest_match != nullptr) {
                ::CoTaskMemFree(closest_match);
            }
            if (support_hr == S_OK) {
                UINT32 default_period = 0;
                UINT32 fundamental_period = 0;
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
        WAVEFORMATEX* closest_match = nullptr;
        const HRESULT support_hr = audio_client->IsFormatSupported(
            AUDCLNT_SHAREMODE_SHARED,
            stream_format,
            &closest_match);
        if (closest_match != nullptr) {
            ::CoTaskMemFree(closest_match);
        }
        if (support_hr != S_OK) {
            log_error_fmt("WASAPI playback: requested format unsupported: {}", hresult_hex(support_hr));
            signal_start_state(start_state,
                support_hr == AUDCLNT_E_UNSUPPORTED_FORMAT
                    ? AudioError::FormatUnsupported
                    : map_hresult(support_hr));
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

            WAVEFORMATEX* closest_match = nullptr;
            const HRESULT support_hr = audio_client->IsFormatSupported(
                AUDCLNT_SHAREMODE_SHARED,
                stream_format,
                &closest_match);
            if (closest_match != nullptr) {
                ::CoTaskMemFree(closest_match);
            }
            if (support_hr != S_OK) {
                signal_start_state(start_state,
                    support_hr == AUDCLNT_E_UNSUPPORTED_FORMAT
                        ? AudioError::FormatUnsupported
                        : map_hresult(support_hr));
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
        signal_start_state(start_state, map_hresult(hr));
        return;
    }

    hr = audio_client->SetEventHandle(static_cast<HANDLE>(audio_event_));
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: SetEventHandle failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_hresult(hr));
        return;
    }

    UINT32 buffer_frames = 0;
    hr = audio_client->GetBufferSize(&buffer_frames);
    if (FAILED(hr) || buffer_frames == 0) {
        log_error_fmt("WASAPI playback: GetBufferSize failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_hresult(hr));
        return;
    }
    log_debug_fmt("WASAPI playback stream ready: device={} mode={} period_frames={} buffer_frames={} block_align={} rate={} channels={} bits={}",
        device_id, use_client3 ? "IAudioClient3" : "IAudioClient", period_frames, buffer_frames,
        stream_format->nBlockAlign, stream_format->nSamplesPerSec, stream_format->nChannels,
        stream_format->wBitsPerSample);

    IAudioRenderClient* raw_render_client = nullptr;
    hr = audio_client->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(&raw_render_client));
    if (FAILED(hr) || raw_render_client == nullptr) {
        log_error_fmt("WASAPI playback: GetService(IAudioRenderClient) failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_hresult(hr));
        return;
    }
    ComPtr<IAudioRenderClient> render_client(raw_render_client);

    // Start 之前先用静音预填充 endpoint。这样能在 start() 返回前
    // 避免确定性的启动 underrun，且不会提前触发应用回调。
    BYTE* data = nullptr;
    hr = render_client->GetBuffer(buffer_frames, &data);
    if (FAILED(hr) || data == nullptr) {
        log_error_fmt("WASAPI playback: initial GetBuffer failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_hresult(hr));
        return;
    }
    std::fill_n(data, static_cast<std::size_t>(buffer_frames) * stream_format->nBlockAlign, BYTE { 0 });
    hr = render_client->ReleaseBuffer(buffer_frames, 0);
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: initial ReleaseBuffer failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_hresult(hr));
        return;
    }

    hr = audio_client->Start();
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: Start failed: {}", hresult_hex(hr));
        signal_start_state(start_state, map_hresult(hr));
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

    for (;;) {
        const DWORD wait_result = ::WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0) {
            log_debug("WASAPI playback stop event received");
            break;
        }
        if (wait_result != WAIT_OBJECT_0 + 1) {
            pending_error_.store(AudioError::BackendFailed, std::memory_order_release);
            ::SetEvent(static_cast<HANDLE>(error_event_));
            break;
        }

        UINT32 padding_frames = 0;
        hr = audio_client->GetCurrentPadding(&padding_frames);
        if (FAILED(hr)) {
            pending_error_.store(map_hresult(hr), std::memory_order_release);
            ::SetEvent(static_cast<HANDLE>(error_event_));
            break;
        }
        if (padding_frames > buffer_frames) {
            pending_error_.store(AudioError::BackendFailed, std::memory_order_release);
            ::SetEvent(static_cast<HANDLE>(error_event_));
            break;
        }

        const UINT32 available_frames = buffer_frames - padding_frames;
        if (available_frames == 0) {
            continue;
        }

        data = nullptr;
        hr = render_client->GetBuffer(available_frames, &data);
        if (FAILED(hr) || data == nullptr) {
            pending_error_.store(map_hresult(hr), std::memory_order_release);
            ::SetEvent(static_cast<HANDLE>(error_event_));
            break;
        }

        const std::size_t output_bytes =
            static_cast<std::size_t>(available_frames) * stream_format->nBlockAlign;
        std::span<std::byte> output(
            reinterpret_cast<std::byte*>(data), output_bytes);

        std::uint32_t written_frames = 0;
        try {
            written_frames = frame_callback_(output);
        } catch (const std::exception& e) {
            log_error_fmt("WASAPI playback callback exception: {}", e.what());
            written_frames = 0;
            pending_error_.store(AudioError::BackendFailed, std::memory_order_release);
        } catch (...) {
            log_error("WASAPI playback callback exception: unknown exception");
            written_frames = 0;
            pending_error_.store(AudioError::BackendFailed, std::memory_order_release);
        }

        if (written_frames > available_frames) {
            log_error_fmt(
                "WASAPI playback callback returned {} frames, but only {} are available",
                written_frames,
                available_frames);
            written_frames = 0;
            pending_error_.store(AudioError::InvalidArgument, std::memory_order_release);
        }

        const std::size_t written_bytes =
            static_cast<std::size_t>(written_frames) * stream_format->nBlockAlign;
        if (written_bytes < output.size()) {
            std::fill(output.begin() + static_cast<std::ptrdiff_t>(written_bytes), output.end(), std::byte { 0 });
        }

        hr = render_client->ReleaseBuffer(available_frames, 0);
        if (FAILED(hr)) {
            pending_error_.store(map_hresult(hr), std::memory_order_release);
            ::SetEvent(static_cast<HANDLE>(error_event_));
            break;
        }

        if (pending_error_.load(std::memory_order_acquire) != AudioError::None) {
            ::SetEvent(static_cast<HANDLE>(error_event_));
            break;
        }
    }

    (void)audio_client->Stop();
    running_.store(false, std::memory_order_release);
    log_debug("WASAPI playback audio thread exited");
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
