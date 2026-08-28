#include "audio/capture/wasapi/wasapi_audio_capture.h"

#include "aqua/audio/devices/audio_device_manager.h"
#include "audio/wasapi/wasapi_com.h"
#include "aqua/logger/logger.h"

// WASAPI / 多媒体头文件有意保持为后端私有。
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>


namespace aqua::audio::wasapi {
namespace {

class ScopedHandle final {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE handle) noexcept
        : handle_(handle)
    {
    }

    ~ScopedHandle()
    {
        reset();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }

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

    // 交出所有权但不关闭底层 HANDLE。
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
            log_warn_fmt("WASAPI: AvSetMmThreadCharacteristicsW(Pro Audio) failed: {}", ::GetLastError());
        } else {
            log_debug("WASAPI capture: MMCSS Pro Audio task registered");
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

    [[nodiscard]] bool active() const noexcept { return handle_ != nullptr; }

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

[[nodiscard]] AudioError map_start_hresult(HRESULT hr) noexcept
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

[[nodiscard]] AudioError map_runtime_hresult(HRESULT hr) noexcept
{
    switch (hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case AUDCLNT_E_RESOURCES_INVALIDATED:
        return AudioError::DeviceDisconnected;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return AudioError::BackendFailed;
    default:
        return AudioError::BackendFailed;
    }
}

[[nodiscard]] EDataFlow to_data_flow(AudioCaptureSource source) noexcept
{
    switch (source) {
    case AudioCaptureSource::INPUT_DEVICE:
        return eCapture;
    case AudioCaptureSource::OUTPUT_LOOPBACK:
        return eRender;
    }
    return EDataFlow(-1);
}

[[nodiscard]] bool guid_equal(const GUID& lhs, const GUID& rhs) noexcept
{
    return ::IsEqualGUID(lhs, rhs) != FALSE;
}

[[nodiscard]] std::optional<AudioEncoding> audio_encoding_from_wave_format(
    const WAVEFORMATEX& format,
    std::uint16_t& container_bits) noexcept
{
    container_bits = format.wBitsPerSample;

    GUID subformat = GUID_NULL;
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            return std::nullopt;
        }
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        subformat = extensible.SubFormat;
        container_bits = format.wBitsPerSample;
    } else if (format.wFormatTag == WAVE_FORMAT_PCM) {
        subformat = KSDATAFORMAT_SUBTYPE_PCM;
    } else if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        subformat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    } else {
        return std::nullopt;
    }

    if (guid_equal(subformat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && container_bits == 32) {
        return AudioEncoding::PCM_F32LE;
    }

    if (!guid_equal(subformat, KSDATAFORMAT_SUBTYPE_PCM)) {
        return std::nullopt;
    }

    switch (container_bits) {
    case 8:
        return AudioEncoding::PCM_U8;
    case 16:
        return AudioEncoding::PCM_S16LE;
    case 24:
        return AudioEncoding::PCM_S24LE;
    case 32:
        return AudioEncoding::PCM_S32LE;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<AudioFormat> audio_format_from_wave_format(
    const WAVEFORMATEX& format) noexcept
{
    if (format.nChannels == 0 || format.nSamplesPerSec == 0) {
        return std::nullopt;
    }

    std::uint16_t container_bits = 0;
    const auto encoding = audio_encoding_from_wave_format(format, container_bits);
    if (!encoding) {
        return std::nullopt;
    }

    const std::uint32_t channels = format.nChannels;
    const std::uint32_t sample_rate = format.nSamplesPerSec;
    AudioFormat result {
        .encoding = *encoding,
        .channels = channels,
        .sample_rate = sample_rate,
    };

    if (!result.is_valid()) {
        return std::nullopt;
    }

    if (format.nBlockAlign != result.frame_bytes()) {
        // Aqua 当前只暴露 packed PCM 语义。因此 24 有效位/32 容器的
        // extensible 流被有意表示为 S32LE，而不是 S24LE。
        return std::nullopt;
    }

    return result;
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

[[nodiscard]] WaveFormatStorage make_requested_format(const AudioFormat& format) noexcept
{
    WaveFormatStorage result;
    const bool use_extensible = format.channels > 2;

    if (!use_extensible) {
        result.basic.nChannels = static_cast<WORD>(format.channels);
        result.basic.nSamplesPerSec = format.sample_rate;
        result.basic.wBitsPerSample = static_cast<WORD>(format.bytes_per_sample() * 8U);
        result.basic.nBlockAlign = static_cast<WORD>(format.frame_bytes());
        result.basic.nAvgBytesPerSec = result.basic.nSamplesPerSec * result.basic.nBlockAlign;
        result.basic.cbSize = 0;
        switch (format.encoding) {
        case AudioEncoding::PCM_F32LE:
            result.basic.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            break;
        case AudioEncoding::PCM_U8:
        case AudioEncoding::PCM_S16LE:
        case AudioEncoding::PCM_S24LE:
        case AudioEncoding::PCM_S32LE:
            result.basic.wFormatTag = WAVE_FORMAT_PCM;
            break;
        case AudioEncoding::INVALID:
            result.basic.wFormatTag = WAVE_FORMAT_UNKNOWN;
            break;
        }
        return result;
    }

    result.is_extensible = true;
    auto& extensible = result.extensible;
    extensible.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    extensible.Format.nChannels = static_cast<WORD>(format.channels);
    extensible.Format.nSamplesPerSec = format.sample_rate;
    extensible.Format.wBitsPerSample = static_cast<WORD>(format.bytes_per_sample() * 8U);
    extensible.Format.nBlockAlign = static_cast<WORD>(format.frame_bytes());
    extensible.Format.nAvgBytesPerSec = extensible.Format.nSamplesPerSec * extensible.Format.nBlockAlign;
    extensible.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    extensible.dwChannelMask = 0;

    switch (format.encoding) {
    case AudioEncoding::PCM_F32LE:
        extensible.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        extensible.Samples.wValidBitsPerSample = 32;
        break;
    case AudioEncoding::PCM_U8:
    case AudioEncoding::PCM_S16LE:
    case AudioEncoding::PCM_S24LE:
    case AudioEncoding::PCM_S32LE:
        extensible.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        extensible.Samples.wValidBitsPerSample = extensible.Format.wBitsPerSample;
        break;
    case AudioEncoding::INVALID:
        extensible.SubFormat = GUID_NULL;
        break;
    }

    return result;
}

} // namespace

struct WasapiAudioCapture::StartState {
    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    AudioError result = AudioError::BackendFailed;
};

void WasapiAudioCapture::signal_start_state(
    const std::shared_ptr<StartState>& state, AudioError result) noexcept
{
    {
        std::lock_guard lock(state->mutex);
        state->result = result;
        state->completed = true;
    }
    state->cv.notify_one();
}

WasapiAudioCapture::WasapiAudioCapture(AudioDeviceManager& device_manager)
    : device_manager_(device_manager)
{
    log_debug("WASAPI capture backend instance created");
}

WasapiAudioCapture::~WasapiAudioCapture()
{
    stop();
}

std::expected<void, AudioError> WasapiAudioCapture::start(
    const AudioCaptureConfig& config,
    AudioCaptureCallback frame_callback,
    AudioCaptureEventCallback event_callback) noexcept
{
    log_debug_fmt("WASAPI capture config: source={} device={} format={}ch/{}Hz/enc={} format_requested={} buffer_frames={}",
        static_cast<int>(config.source),
        config.device ? config.device->value() : std::string("default"),
        config.format ? config.format->channels : 0,
        config.format ? config.format->sample_rate : 0,
        config.format ? static_cast<int>(config.format->encoding) : static_cast<int>(AudioEncoding::INVALID),
        config.format.has_value(),
        config.frames_per_buffer);

    if (running_.load(std::memory_order_acquire) || audio_thread_.joinable() || event_thread_.joinable()) {
        log_error("WASAPI capture: start rejected because capture is already running");
        return std::unexpected(AudioError::AlreadyRunning);
    }

    if (!frame_callback) {
        log_error("WASAPI capture: start rejected because frame callback is empty");
        return std::unexpected(AudioError::InvalidArgument);
    }

    if (config.format && !config.format->is_valid()) {
        log_error("WASAPI capture: start rejected because requested format is invalid");
        return std::unexpected(AudioError::InvalidArgument);
    }

    AudioDeviceDirection direction = AudioDeviceDirection::NONE;
    switch (config.source) {
    case AudioCaptureSource::INPUT_DEVICE:
        direction = AudioDeviceDirection::INPUT;
        break;
    case AudioCaptureSource::OUTPUT_LOOPBACK:
        direction = AudioDeviceDirection::OUTPUT;
        break;
    default:
        log_error_fmt("WASAPI capture: invalid capture source={}", static_cast<int>(config.source));
        return std::unexpected(AudioError::InvalidArgument);
    }

    const auto resolved = device_manager_.resolve(direction, config.device);
    if (!resolved) {
        log_error_fmt("WASAPI capture: device resolution failed: {}", audio_error_name(resolved.error()));
        return std::unexpected(resolved.error());
    }
    log_debug_fmt("WASAPI capture device resolved: id='{}' name='{}' direction={} default={}",
        resolved->id.value(), resolved->name, static_cast<int>(resolved->direction), resolved->is_default);

    ScopedHandle stop_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ScopedHandle audio_event(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    ScopedHandle error_event(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!stop_event || !audio_event || !error_event) {
        log_error_fmt("WASAPI capture: failed to create synchronization events (GetLastError={})", ::GetLastError());
        return std::unexpected(AudioError::BackendFailed);
    }

    stop_event_ = stop_event.get();
    audio_event_ = audio_event.get();
    error_event_ = error_event.get();

    // 采集后端在本实例生命周期内取得这些句柄的所有权。
    (void)stop_event.release();
    (void)audio_event.release();
    (void)error_event.release();

    frame_callback_ = std::move(frame_callback);
    event_callback_ = std::move(event_callback);
    pending_error_.store(AudioError::None, std::memory_order_release);

    const auto start_state = std::make_shared<StartState>();
    try {
        audio_thread_ = std::thread(
            &WasapiAudioCapture::audio_thread_main,
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
        log_debug_fmt("WASAPI capture initialization failed before event thread start: {}", audio_error_name(start_state->result));
        stop();
        return std::unexpected(start_state->result);
    }

    try {
        event_thread_ = std::thread(&WasapiAudioCapture::event_thread_main, this);
        log_debug("WASAPI capture error-event thread started");
    } catch (const std::exception& e) {
        log_error_fmt("WASAPI capture: failed to start error event thread: {}", e.what());
        stop();
        return std::unexpected(AudioError::BackendFailed);
    } catch (...) {
        log_error("WASAPI capture: failed to start error event thread");
        stop();
        return std::unexpected(AudioError::BackendFailed);
    }

    log_info_fmt("WASAPI capture started: device={} format={}ch/{}Hz buffer_frames={}",
        resolved->id.value(), info_.format.channels, info_.format.sample_rate,
        info_.frames_per_buffer);
    return {};
}

const AudioCaptureInfo& WasapiAudioCapture::info() const noexcept
{
    return info_;
}

bool WasapiAudioCapture::is_running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

void WasapiAudioCapture::stop() noexcept
{
    log_debug("WASAPI capture stop requested");
    if (stop_event_ != nullptr) {
        ::SetEvent(stop_event_);
    }

    if (audio_thread_.joinable()) {
        if (audio_thread_.get_id() == std::this_thread::get_id()) {
            // 公共契约禁止在音频回调里调用 stop()。若有缺陷的调用方违反该契约，
            // 这里避免死锁。
            return;
        }
        audio_thread_.join();
    }

    if (error_event_ != nullptr) {
        ::SetEvent(error_event_);
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
        ::CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    if (audio_event_ != nullptr) {
        ::CloseHandle(audio_event_);
        audio_event_ = nullptr;
    }
    if (error_event_ != nullptr) {
        ::CloseHandle(error_event_);
        error_event_ = nullptr;
    }
}

void WasapiAudioCapture::audio_thread_main(
    std::string device_id,
    AudioCaptureConfig config,
    std::shared_ptr<StartState> start_state) noexcept
{
    try {
        audio_thread_main_impl(std::move(device_id), std::move(config), start_state);
    } catch (const std::exception& e) {
        log_error_fmt("WASAPI capture audio thread exception: {}", e.what());
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
                ::SetEvent(error_event_);
            }
        }
    } catch (...) {
        log_error("WASAPI capture audio thread exception: unknown exception");
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
                ::SetEvent(error_event_);
            }
        }
    }
}

void WasapiAudioCapture::audio_thread_main_impl(
    std::string device_id,
    AudioCaptureConfig config,
    std::shared_ptr<StartState> start_state)
{
    ScopedComInitialization com;
    if (!com.usable()) {
        signal_start_state(start_state, AudioError::BackendFailed);
        return;
    }

    ScopedMmcssTask mmcss;

    ComPtr<IMMDeviceEnumerator> enumerator;
    {
        IMMDeviceEnumerator* raw = nullptr;
        const HRESULT hr = ::CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&raw));
        if (FAILED(hr) || raw == nullptr) {
            log_error_fmt("WASAPI capture: CoCreateInstance(MMDeviceEnumerator) failed: {}", hresult_hex(hr));
            signal_start_state(start_state, map_start_hresult(hr));
            return;
        }
        enumerator.reset(raw);
    }

    const EDataFlow flow = to_data_flow(config.source);
    if (flow != eCapture && flow != eRender) {
        signal_start_state(start_state, AudioError::InvalidArgument);
        return;
    }

    const std::wstring wide_id = [&] {
        int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            device_id.data(), static_cast<int>(device_id.size()), nullptr, 0);
        if (length <= 0) {
            return std::wstring {};
        }
        std::wstring result(static_cast<std::size_t>(length), L'\0');
        if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                device_id.data(), static_cast<int>(device_id.size()), result.data(), length) <= 0) {
            return std::wstring {};
        }
        return result;
    }();
    if (wide_id.empty()) {
        signal_start_state(start_state, AudioError::InvalidArgument);
        return;
    }

    ComPtr<IMMDevice> device;
    {
        IMMDevice* raw = nullptr;
        const HRESULT hr = enumerator->GetDevice(wide_id.c_str(), &raw);
        if (FAILED(hr) || raw == nullptr) {
            signal_start_state(start_state, AudioError::DeviceNotFound);
            return;
        }
        device.reset(raw);
    }

    ComPtr<IAudioClient> audio_client;
    {
        IAudioClient* raw = nullptr;
        const HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&raw));
        if (FAILED(hr) || raw == nullptr) {
            log_error_fmt("WASAPI capture: IMMDevice::Activate(IAudioClient) failed: {}", hresult_hex(hr));
            signal_start_state(start_state, map_start_hresult(hr));
            return;
        }
        audio_client.reset(raw);
    }

    WAVEFORMATEX* raw_mix_format = nullptr;
    const HRESULT mix_hr = audio_client->GetMixFormat(&raw_mix_format);
    if (FAILED(mix_hr) || raw_mix_format == nullptr) {
        log_error_fmt("WASAPI capture: GetMixFormat failed: {}", hresult_hex(mix_hr));
        signal_start_state(start_state, map_start_hresult(mix_hr));
        return;
    }
    std::unique_ptr<WAVEFORMATEX, decltype(&::CoTaskMemFree)> mix_format(raw_mix_format, &::CoTaskMemFree);
    if (const auto mix_audio_format = audio_format_from_wave_format(*mix_format)) {
        log_debug_fmt("WASAPI capture device mix format: {}ch/{}Hz/enc={} block_align={} bits={}",
            mix_audio_format->channels, mix_audio_format->sample_rate,
            static_cast<int>(mix_audio_format->encoding), mix_format->nBlockAlign,
            mix_format->wBitsPerSample);
    } else {
        log_debug_fmt("WASAPI capture device mix format is not representable by AudioFormat: tag={} channels={} rate={} bits={}",
            mix_format->wFormatTag, mix_format->nChannels, mix_format->nSamplesPerSec,
            mix_format->wBitsPerSample);
    }

    WAVEFORMATEX* stream_format = mix_format.get();
    std::optional<WaveFormatStorage> requested_format;
    if (config.format) {
        requested_format = make_requested_format(*config.format);
        stream_format = requested_format->get();

        WAVEFORMATEX* closest_match = nullptr;
        const HRESULT support_hr = audio_client->IsFormatSupported(
            AUDCLNT_SHAREMODE_SHARED,
            stream_format,
            &closest_match);
        if (closest_match != nullptr) {
            ::CoTaskMemFree(closest_match);
        }
        if (support_hr != S_OK) {
            log_error_fmt("WASAPI capture: requested format unsupported: {}", hresult_hex(support_hr));
            signal_start_state(start_state, support_hr == AUDCLNT_E_UNSUPPORTED_FORMAT
                    ? AudioError::FormatUnsupported
                    : map_start_hresult(support_hr));
            return;
        }
    }

    const auto actual_format = audio_format_from_wave_format(*stream_format);
    if (actual_format) {
        log_debug_fmt("WASAPI capture stream format selected: {}ch/{}Hz/enc={}",
            actual_format->channels, actual_format->sample_rate, static_cast<int>(actual_format->encoding));
    }
    if (!actual_format) {
        log_error("WASAPI capture: stream WAVEFORMAT cannot be represented by AudioFormat");
        signal_start_state(start_state, AudioError::FormatUnsupported);
        return;
    }
    if (config.format && *actual_format != *config.format) {
        log_error("WASAPI capture: backend format differs from requested format without conversion");
        signal_start_state(start_state, AudioError::FormatUnsupported);
        return;
    }

    DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (config.source == AudioCaptureSource::OUTPUT_LOOPBACK) {
        stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    // Shared + 事件驱动模式必须把 buffer duration 与 period 都设为 0，
    // 让 WASAPI 分配引擎所需的最小 shared 缓冲。
    log_debug_fmt("WASAPI capture initializing shared event stream: source={} loopback={} requested_buffer_frames={} flags=0x{:08X}",
        static_cast<int>(config.source), config.source == AudioCaptureSource::OUTPUT_LOOPBACK,
        config.frames_per_buffer, static_cast<unsigned>(stream_flags));
    const HRESULT init_hr = audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        stream_flags,
        0,
        0,
        stream_format,
        nullptr);
    if (FAILED(init_hr)) {
        log_error_fmt("WASAPI capture: IAudioClient::Initialize failed: {}", hresult_hex(init_hr));
        signal_start_state(start_state, map_start_hresult(init_hr));
        return;
    }

    const HRESULT event_hr = audio_client->SetEventHandle(audio_event_);
    if (FAILED(event_hr)) {
        log_error_fmt("WASAPI capture: SetEventHandle failed: {}", hresult_hex(event_hr));
        signal_start_state(start_state, map_start_hresult(event_hr));
        return;
    }

    UINT32 buffer_frames = 0;
    const HRESULT buffer_hr = audio_client->GetBufferSize(&buffer_frames);
    if (FAILED(buffer_hr) || buffer_frames == 0) {
        log_error_fmt("WASAPI capture: GetBufferSize failed: {}", hresult_hex(buffer_hr));
        signal_start_state(start_state, map_start_hresult(buffer_hr));
        return;
    }

    IAudioCaptureClient* raw_capture_client = nullptr;
    const HRESULT service_hr = audio_client->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(&raw_capture_client));
    if (FAILED(service_hr) || raw_capture_client == nullptr) {
        log_error_fmt("WASAPI capture: GetService(IAudioCaptureClient) failed: {}", hresult_hex(service_hr));
        signal_start_state(start_state, map_start_hresult(service_hr));
        return;
    }
    ComPtr<IAudioCaptureClient> capture_client(raw_capture_client);

    const std::size_t silence_bytes = actual_format->bytes_for_frames(buffer_frames);
    std::vector<std::byte> silence;
    try {
        silence.resize(silence_bytes);
    } catch (...) {
        signal_start_state(start_state, AudioError::BackendFailed);
        return;
    }

    AudioCaptureInfo actual_info;
    actual_info.format = *actual_format;
    actual_info.frames_per_buffer = buffer_frames;
    (void)config.frames_per_buffer;

    log_debug_fmt("WASAPI capture stream ready: format={}ch/{}Hz buffer_frames={} source={} device={}",
        actual_info.format.channels, actual_info.format.sample_rate, actual_info.frames_per_buffer,
        static_cast<int>(config.source), device_id);

    const HRESULT start_hr = audio_client->Start();
    if (FAILED(start_hr)) {
        log_error_fmt("WASAPI capture: IAudioClient::Start failed: {}", hresult_hex(start_hr));
        signal_start_state(start_state, map_start_hresult(start_hr));
        return;
    }

    info_ = actual_info;
    log_debug_fmt("WASAPI capture starting stream: device={} format={}ch/{}Hz buffer_frames={} loopback={}",
        device_id, actual_info.format.channels, actual_info.format.sample_rate,
        actual_info.frames_per_buffer, config.source == AudioCaptureSource::OUTPUT_LOOPBACK);
    running_.store(true, std::memory_order_release);
    signal_start_state(start_state, AudioError::None);

    HANDLE wait_handles[2] = { stop_event_, audio_event_ };
    bool stopping = false;
    while (!stopping) {
        const DWORD wait_result = ::WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0) {
            stopping = true;
            break;
        }
        if (wait_result != WAIT_OBJECT_0 + 1) {
            pending_error_.store(AudioError::BackendFailed, std::memory_order_release);
            if (error_event_ != nullptr) {
                ::SetEvent(error_event_);
            }
            break;
        }

        // 一个事件可能对应多个 packet，因此要把 client 缓冲完全排空。
        for (;;) {
            UINT32 packet_frames = 0;
            HRESULT hr = capture_client->GetNextPacketSize(&packet_frames);
            if (FAILED(hr)) {
                const AudioError error = map_runtime_hresult(hr);
                pending_error_.store(error, std::memory_order_release);
                if (error_event_ != nullptr) {
                    ::SetEvent(error_event_);
                }
                stopping = true;
                break;
            }

            if (packet_frames == 0) {
                break;
            }

            BYTE* data = nullptr;
            UINT32 frames_to_read = 0;
            DWORD flags = 0;
            UINT64 device_position = 0;
            UINT64 qpc_position = 0;
            hr = capture_client->GetBuffer(
                &data,
                &frames_to_read,
                &flags,
                &device_position,
                &qpc_position);
            if (FAILED(hr)) {
                const AudioError error = map_runtime_hresult(hr);
                pending_error_.store(error, std::memory_order_release);
                if (error_event_ != nullptr) {
                    ::SetEvent(error_event_);
                }
                stopping = true;
                break;
            }

            if (frames_to_read == 0) {
                capture_client->ReleaseBuffer(0);
                continue;
            }

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const std::size_t byte_count = actual_format->bytes_for_frames(frames_to_read);
            std::span<const std::byte> payload;
            if (silent) {
                if (byte_count > silence.size()) {
                    capture_client->ReleaseBuffer(frames_to_read);
                    pending_error_.store(AudioError::BackendFailed, std::memory_order_release);
                    if (error_event_ != nullptr) {
                        ::SetEvent(error_event_);
                    }
                    stopping = true;
                    break;
                }
                payload = std::span<const std::byte>(silence.data(), byte_count);
            } else {
                payload = std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(data), byte_count);
            }

            (void)device_position;
            (void)qpc_position;

            AudioBlock block { payload };
            frame_callback_(block);

            hr = capture_client->ReleaseBuffer(frames_to_read);
            if (FAILED(hr)) {
                const AudioError error = map_runtime_hresult(hr);
                pending_error_.store(error, std::memory_order_release);
                if (error_event_ != nullptr) {
                    ::SetEvent(error_event_);
                }
                stopping = true;
                break;
            }
        }
    }

    (void)audio_client->Stop();
    running_.store(false, std::memory_order_release);
    log_debug("WASAPI capture audio thread exited");
}

void WasapiAudioCapture::event_thread_main() noexcept
{
    HANDLE wait_handles[2] = { stop_event_, error_event_ };

    for (;;) {
        const DWORD wait_result = ::WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0) {
            log_debug("WASAPI capture error-event thread stopped by shutdown");
            return;
        }
        if (wait_result != WAIT_OBJECT_0 + 1) {
            log_error_fmt("WASAPI capture error-event thread wait failed: result={}", wait_result);
            return;
        }

        const AudioError error = pending_error_.exchange(AudioError::None, std::memory_order_acq_rel);
        if (error == AudioError::None) {
            continue;
        }

        log_debug_fmt("WASAPI capture error event thread exiting after error={}",
            audio_error_name(error));
        if (event_callback_) {
            event_callback_(error);
        } else {
            log_warn_fmt("WASAPI capture runtime error: {}", static_cast<int>(error));
        }
        return;
    }
}

} // namespace aqua::audio::wasapi
