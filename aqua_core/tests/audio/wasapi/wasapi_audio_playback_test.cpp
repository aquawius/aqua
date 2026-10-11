#include "aqua/audio/audio_format.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/playback/audio_playback.h"
#include "audio/public/wasapi/wasapi_engine_period.h"
#include "audio/public/wasapi/wasapi_lifecycle.h"

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

class ScopedComObject final {
public:
    explicit ScopedComObject(IUnknown* value) noexcept
        : value_(value)
    {
    }
    ~ScopedComObject()
    {
        if (value_ != nullptr) {
            value_->Release();
        }
    }

    ScopedComObject(const ScopedComObject&) = delete;
    ScopedComObject& operator=(const ScopedComObject&) = delete;

private:
    IUnknown* value_ = nullptr;
};

class ScopedComInitialization final {
public:
    ScopedComInitialization() noexcept
    {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(hr);
        usable_ = initialized_ || hr == RPC_E_CHANGED_MODE;
    }

    ~ScopedComInitialization()
    {
        if (initialized_) {
            ::CoUninitialize();
        }
    }

    ScopedComInitialization(const ScopedComInitialization&) = delete;
    ScopedComInitialization& operator=(const ScopedComInitialization&) = delete;

    [[nodiscard]] bool usable() const noexcept { return usable_; }

private:
    bool initialized_ = false;
    bool usable_ = false;
};

std::optional<aqua::audio::AudioFormat> get_mix_format(
    const aqua::audio::AudioDevice& device)
{
    ScopedComInitialization com;
    if (!com.usable()) {
        return std::nullopt;
    }

    IMMDeviceEnumerator* raw_enumerator = nullptr;
    if (FAILED(::CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&raw_enumerator)))) {
        return std::nullopt;
    }
    ScopedComObject enumerator_guard(raw_enumerator);
    IMMDeviceEnumerator* enumerator = raw_enumerator;

    int wide_length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        device.id.value().data(), static_cast<int>(device.id.value().size()),
        nullptr, 0);
    if (wide_length <= 0) {
        return std::nullopt;
    }
    std::wstring wide_id(static_cast<std::size_t>(wide_length), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS,
            device.id.value().data(), static_cast<int>(device.id.value().size()),
            wide_id.data(), wide_length)
        <= 0) {
        return std::nullopt;
    }

    IMMDevice* raw_device = nullptr;
    if (FAILED(enumerator->GetDevice(wide_id.c_str(), &raw_device)) || raw_device == nullptr) {
        return std::nullopt;
    }
    ScopedComObject endpoint_guard(raw_device);
    IMMDevice* endpoint = raw_device;

    IAudioClient* raw_client = nullptr;
    if (FAILED(endpoint->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&raw_client)))
        || raw_client == nullptr) {
        return std::nullopt;
    }
    ScopedComObject client_guard(raw_client);
    IAudioClient* client = raw_client;

    WAVEFORMATEX* raw_format = nullptr;
    if (FAILED(client->GetMixFormat(&raw_format)) || raw_format == nullptr) {
        return std::nullopt;
    }
    std::unique_ptr<WAVEFORMATEX, decltype(&::CoTaskMemFree)> format(
        raw_format, &::CoTaskMemFree);

    if (format->nChannels == 0 || format->nSamplesPerSec == 0) {
        return std::nullopt;
    }

    aqua::audio::AudioEncoding encoding;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            return std::nullopt;
        }
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(*format);
        if (::IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && format->wBitsPerSample == 32) {
            encoding = aqua::audio::AudioEncoding::PCM_F32LE;
        } else if (::IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            switch (format->wBitsPerSample) {
            case 8:
                encoding = aqua::audio::AudioEncoding::PCM_U8;
                break;
            case 16:
                encoding = aqua::audio::AudioEncoding::PCM_S16LE;
                break;
            case 24:
                encoding = aqua::audio::AudioEncoding::PCM_S24LE;
                break;
            case 32:
                encoding = aqua::audio::AudioEncoding::PCM_S32LE;
                break;
            default:
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) {
        encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    } else if (format->wFormatTag == WAVE_FORMAT_PCM) {
        switch (format->wBitsPerSample) {
        case 8:
            encoding = aqua::audio::AudioEncoding::PCM_U8;
            break;
        case 16:
            encoding = aqua::audio::AudioEncoding::PCM_S16LE;
            break;
        case 24:
            encoding = aqua::audio::AudioEncoding::PCM_S24LE;
            break;
        case 32:
            encoding = aqua::audio::AudioEncoding::PCM_S32LE;
            break;
        default:
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }

    aqua::audio::AudioFormat result {
        .encoding = encoding,
        .channels = format->nChannels,
        .sample_rate = format->nSamplesPerSec,
    };
    return result.is_valid() ? std::optional(result) : std::nullopt;
}

struct PlaybackStats {
    std::atomic<std::uint64_t> callback_count = 0;
    std::atomic<aqua::audio::AudioError> last_error = aqua::audio::AudioError::None;
    std::uint32_t frame_bytes = 0;
};

auto make_playback_cb(PlaybackStats& stats)
{
    return [&stats](std::span<std::byte> output) noexcept -> std::uint32_t {
        ++stats.callback_count;
        std::fill(output.begin(), output.end(), std::byte { 0 });
        return stats.frame_bytes == 0
            ? 0U
            : static_cast<std::uint32_t>(output.size() / stats.frame_bytes);
    };
}

auto make_playback_event_cb(PlaybackStats& stats)
{
    return [&stats](const aqua::audio::AudioStreamEvent& event) noexcept {
        stats.last_error.store(event.error, std::memory_order_release);
    };
}

} // namespace

TEST(WasapiAudioPlaybackTest, DefaultOutputStartsAndInvokesCallback)
{
    auto manager = aqua::audio::create_device_manager();
    if (!manager) {
        GTEST_SKIP() << "WASAPI device manager is unavailable";
    }
    auto device = manager->default_device(aqua::audio::AudioDeviceDirection::OUTPUT);
    if (!device) {
        GTEST_SKIP() << "No default output device is available";
    }

    const auto format = get_mix_format(*device);
    if (!format) {
        GTEST_SKIP() << "Unable to query the default output mix format";
    }

    auto playback = aqua::audio::create_playback(*manager);
    ASSERT_NE(playback, nullptr);

    PlaybackStats stats;
    stats.frame_bytes = format->frame_bytes();
    aqua::audio::AudioPlaybackConfig config {
        .route = aqua::audio::AudioRoute::follow_system(),
        .format = *format,
        .frames_per_buffer = 0,
    };

    const auto result = playback->start(
        config,
        make_playback_cb(stats),
        make_playback_event_cb(stats));
    ASSERT_TRUE(result) << "start() failed";
    ASSERT_TRUE(playback->is_running());

    // 输出流实际参数：start 成功后可回读（WASAPI shared mode 契约）。
    const auto info = playback->stream_info();
    EXPECT_EQ(info.backend, aqua::audio::AudioStreamInfo::Backend::Wasapi);
    EXPECT_EQ(info.sample_rate, format->sample_rate);
    EXPECT_EQ(info.channels, format->channels);
    EXPECT_GT(info.buffer_capacity_frames, 0U);
    // 激活的 endpoint id 即实际设备（restart 事务的 previous_active_device 来源）。
    EXPECT_FALSE(info.device_id.empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    playback->stop();

    EXPECT_FALSE(playback->is_running());
    // stop 后诊断缓存清零。
    EXPECT_EQ(playback->stream_info().backend, aqua::audio::AudioStreamInfo::Backend::None);
    EXPECT_GT(stats.callback_count.load(), 0U);
    EXPECT_EQ(stats.last_error.load(), aqua::audio::AudioError::None);
}

TEST(WasapiAudioPlaybackTest, SpecifiedOutputStarts)
{
    auto manager = aqua::audio::create_device_manager();
    if (!manager) {
        GTEST_SKIP() << "WASAPI device manager is unavailable";
    }
    const auto devices = manager->enumerate(aqua::audio::AudioDeviceDirection::OUTPUT);
    if (devices.empty()) {
        GTEST_SKIP() << "No output device is available";
    }

    const auto format = get_mix_format(devices.front());
    if (!format) {
        GTEST_SKIP() << "Unable to query the selected output mix format";
    }

    auto playback = aqua::audio::create_playback(*manager);
    ASSERT_NE(playback, nullptr);

    PlaybackStats stats;
    stats.frame_bytes = format->frame_bytes();
    aqua::audio::AudioPlaybackConfig config {
        .route = aqua::audio::AudioRoute::pin(devices.front().id),
        .format = *format,
        .frames_per_buffer = 0,
    };

    const auto result = playback->start(
        config,
        make_playback_cb(stats),
        make_playback_event_cb(stats));
    ASSERT_TRUE(result) << "start() failed for specified device";
    ASSERT_TRUE(playback->is_running());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    playback->stop();

    EXPECT_FALSE(playback->is_running());
    EXPECT_GT(stats.callback_count.load(), 0U);
    EXPECT_EQ(stats.last_error.load(), aqua::audio::AudioError::None);
}

TEST(WasapiAudioPlaybackTest, InvalidFormatIsRejected)
{
    auto manager = aqua::audio::create_device_manager();
    if (!manager) {
        GTEST_SKIP() << "WASAPI device manager is unavailable";
    }
    auto playback = aqua::audio::create_playback(*manager);
    ASSERT_NE(playback, nullptr);

    const aqua::audio::AudioPlaybackConfig config {
        .route = aqua::audio::AudioRoute::follow_system(),
        .format = { },
        .frames_per_buffer = 0,
    };

    PlaybackStats stats;
    const auto result = playback->start(config, make_playback_cb(stats));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), aqua::audio::AudioError::InvalidArgument);
}

TEST(WasapiAudioPlaybackTest, StartWhileRunningIsRejected)
{
    auto manager = aqua::audio::create_device_manager();
    if (!manager) {
        GTEST_SKIP() << "WASAPI device manager is unavailable";
    }
    auto device = manager->default_device(aqua::audio::AudioDeviceDirection::OUTPUT);
    if (!device) {
        GTEST_SKIP() << "No default output device is available";
    }
    const auto format = get_mix_format(*device);
    if (!format) {
        GTEST_SKIP() << "Unable to query output mix format";
    }

    auto playback = aqua::audio::create_playback(*manager);
    ASSERT_NE(playback, nullptr);

    const aqua::audio::AudioPlaybackConfig config {
        .route = aqua::audio::AudioRoute::follow_system(),
        .format = *format,
        .frames_per_buffer = 0,
    };
    PlaybackStats stats;
    ASSERT_TRUE(playback->start(config, make_playback_cb(stats)));

    const auto second = playback->start(config, make_playback_cb(stats));
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error(), aqua::audio::AudioError::AlreadyRunning);

    playback->stop();
}

TEST(WasapiAudioPlaybackTest, StartWithNullCallbackIsRejected)
{
    auto manager = aqua::audio::create_device_manager();
    if (!manager) {
        GTEST_SKIP() << "WASAPI device manager is unavailable";
    }
    auto playback = aqua::audio::create_playback(*manager);
    ASSERT_NE(playback, nullptr);

    const aqua::audio::AudioPlaybackConfig config {
        .route = aqua::audio::AudioRoute::follow_system(),
        .format = aqua::audio::AudioFormat {
            .encoding = aqua::audio::AudioEncoding::PCM_S16LE,
            .channels = 2,
            .sample_rate = 48000,
        },
        .frames_per_buffer = 0,
    };

    const auto result = playback->start(config, nullptr);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), aqua::audio::AudioError::InvalidArgument);
    EXPECT_FALSE(playback->is_running());
}

TEST(WasapiAudioPlaybackTest, StartWithUnknownDeviceFails)
{
    auto manager = aqua::audio::create_device_manager();
    if (!manager) {
        GTEST_SKIP() << "WASAPI device manager is unavailable";
    }
    auto playback = aqua::audio::create_playback(*manager);
    ASSERT_NE(playback, nullptr);

    const aqua::audio::AudioPlaybackConfig config {
        .route = aqua::audio::AudioRoute::pin(aqua::audio::AudioDeviceId { "aqua/nonexistent/device/id" }),
        .format = aqua::audio::AudioFormat {
            .encoding = aqua::audio::AudioEncoding::PCM_S16LE,
            .channels = 2,
            .sample_rate = 48000,
        },
        .frames_per_buffer = 0,
    };

    PlaybackStats stats;
    const auto result = playback->start(config, make_playback_cb(stats));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), aqua::audio::AudioError::DeviceNotFound);
    EXPECT_FALSE(playback->is_running());
}

TEST(WasapiAudioPlaybackTest, StartWithInvalidUtf8DeviceIdIsRejected)
{
    auto manager = aqua::audio::create_device_manager();
    if (!manager) {
        GTEST_SKIP() << "WASAPI device manager is unavailable";
    }
    auto playback = aqua::audio::create_playback(*manager);
    ASSERT_NE(playback, nullptr);

    const aqua::audio::AudioPlaybackConfig config {
        .route = aqua::audio::AudioRoute::pin(aqua::audio::AudioDeviceId { "\xFF\xFE\xFD" }),
        .format = aqua::audio::AudioFormat {
            .encoding = aqua::audio::AudioEncoding::PCM_S16LE,
            .channels = 2,
            .sample_rate = 48000,
        },
        .frames_per_buffer = 0,
    };

    PlaybackStats stats;
    const auto result = playback->start(config, make_playback_cb(stats));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), aqua::audio::AudioError::InvalidArgument);
    EXPECT_FALSE(playback->is_running());
}

TEST(WasapiAudioPlaybackTest, StopBeforeStartIsNoOp)
{
    auto manager = aqua::audio::create_device_manager();
    if (!manager) {
        GTEST_SKIP() << "WASAPI device manager is unavailable";
    }
    auto playback = aqua::audio::create_playback(*manager);
    ASSERT_NE(playback, nullptr);

    EXPECT_FALSE(playback->is_running());
    playback->stop();
    EXPECT_FALSE(playback->is_running());
    playback->stop();
    EXPECT_FALSE(playback->is_running());
}

TEST(WasapiAudioPlaybackTest, CanStartAfterFailedStart)
{
    auto manager = aqua::audio::create_device_manager();
    if (!manager) {
        GTEST_SKIP() << "WASAPI device manager is unavailable";
    }
    auto device = manager->default_device(aqua::audio::AudioDeviceDirection::OUTPUT);
    if (!device) {
        GTEST_SKIP() << "No default output device is available";
    }
    const auto format = get_mix_format(*device);
    if (!format) {
        GTEST_SKIP() << "Unable to query output mix format";
    }

    auto playback = aqua::audio::create_playback(*manager);
    ASSERT_NE(playback, nullptr);

    PlaybackStats stats;
    stats.frame_bytes = format->frame_bytes();

    const aqua::audio::AudioPlaybackConfig bad_config {
        .route = aqua::audio::AudioRoute::pin(aqua::audio::AudioDeviceId { "aqua/nonexistent/device/id" }),
        .format = *format,
        .frames_per_buffer = 0,
    };
    const auto bad = playback->start(bad_config, make_playback_cb(stats));
    ASSERT_FALSE(bad);

    const aqua::audio::AudioPlaybackConfig good_config {
        .route = aqua::audio::AudioRoute::follow_system(),
        .format = *format,
        .frames_per_buffer = 0,
    };
    const auto good = playback->start(good_config, make_playback_cb(stats));
    ASSERT_TRUE(good)
        << "start after failed start failed, error=" << static_cast<int>(good.error());

    EXPECT_TRUE(playback->is_running());
    playback->stop();
    EXPECT_FALSE(playback->is_running());
}

TEST(WasapiAudioPlaybackTest, StartWithHugeFramesPerBufferSucceeds)
{
    auto manager = aqua::audio::create_device_manager();
    if (!manager) {
        GTEST_SKIP() << "WASAPI device manager is unavailable";
    }
    auto device = manager->default_device(aqua::audio::AudioDeviceDirection::OUTPUT);
    if (!device) {
        GTEST_SKIP() << "No default output device is available";
    }
    const auto format = get_mix_format(*device);
    if (!format) {
        GTEST_SKIP() << "Unable to query output mix format";
    }

    auto playback = aqua::audio::create_playback(*manager);
    ASSERT_NE(playback, nullptr);

    PlaybackStats stats;
    stats.frame_bytes = format->frame_bytes();

    // 超大的请求缓冲必须被 clamp 到引擎支持的范围，而不是被拒绝或溢出。
    const aqua::audio::AudioPlaybackConfig config {
        .route = aqua::audio::AudioRoute::follow_system(),
        .format = *format,
        .frames_per_buffer = 0xFFFFFFFFu,
    };
    const auto result = playback->start(config, make_playback_cb(stats));
    ASSERT_TRUE(result) << "start with huge frames_per_buffer failed";
    EXPECT_TRUE(playback->is_running());

    playback->stop();
    EXPECT_FALSE(playback->is_running());
}

// `frames_per_buffer` 是 uint32，而 AAudio 的容量参数是 int32_t：超大请求直接
// static_cast<int32_t> 会变成**负数**（0xFFFFFFFF -> -1），于是"超大请求"退化成非法小缓冲，
// 后端要么拒开流、要么行为未定义。这与 WASAPI 侧的契约相反（见上面
// StartWithHugeFramesPerBufferSucceeds：超大请求必须被钳制、会话照常起来）。
//
// 折算抽成平台中立的 audio_playback_config.h 自由函数，正是为了让这条防线在本机也能被钉住 ——
// AAudio 后端只在 Android 编译，本仓没有设备可跑它；本用例就是那条契约的姊妹用例。
TEST(WasapiAudioPlaybackTest, FramesPerBufferIsClampedBeforeNarrowingToInt32)
{
    constexpr auto kInt32Max = (std::numeric_limits<std::int32_t>::max)();

    // 反向用例（旧实现正是在这里给出负数）：越过 int32 上限的请求必须被钳到上限。
    EXPECT_GT(aqua::audio::frames_per_buffer_as_int32(0xFFFFFFFFu), 0);
    EXPECT_EQ(aqua::audio::frames_per_buffer_as_int32(0xFFFFFFFFu), kInt32Max);
    EXPECT_EQ(aqua::audio::frames_per_buffer_as_int32(0x80000000u), kInt32Max);

    // 边界：恰好能在 int32 里表示的最大值原样保留，多 1 才钳。
    EXPECT_EQ(aqua::audio::frames_per_buffer_as_int32(
                  static_cast<std::uint32_t>(kInt32Max)),
        kInt32Max);
    EXPECT_EQ(aqua::audio::frames_per_buffer_as_int32(
                  static_cast<std::uint32_t>(kInt32Max) + 1u),
        kInt32Max);

    // 常规值不受影响（0 在 AAudio 侧表示"不设置容量"，由调用点分支处理）。
    EXPECT_EQ(aqua::audio::frames_per_buffer_as_int32(0u), 0);
    EXPECT_EQ(aqua::audio::frames_per_buffer_as_int32(480u), 480);
}

// `choose_engine_period`：回放与采集共用的"请求帧数 -> 引擎周期"折算。它原先是回放 TU 里的
// 匿名函数（拿不到任何单测），搬到 audio/public/wasapi/ 之后终于可以直测 —— 三条契约
// （0 = 最小周期 / 请求值取最接近的 fundamental 倍数 / 必须落在 [min, max] 内）里，
// 前两条默认值就靠它。
//
// 设备取值取自实机（48kHz）：default=480 / fundamental=1 / min=336 / max=480；
// MS 文档的示例（44.1kHz）是 default=448 / fundamental=4 / min=48 / max=448。
TEST(WasapiAudioPlaybackTest, EnginePeriodSelectionFollowsEngineContracts)
{
    // 0 = **设备最小周期**（不是引擎默认周期）：这是本选项默认行为的核心。
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(0u, 1u, 336u, 480u), 336u);
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(0u, 4u, 48u, 448u), 48u);

    // 显式请求：引擎默认周期仍要得到（"想退回默认就显式传它"）。
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(480u, 1u, 336u, 480u), 480u);
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(448u, 4u, 48u, 448u), 448u);

    // 请求值取**最接近**的 fundamental 倍数（四舍五入到倍数，不是截断）。
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(100u, 4u, 48u, 448u), 100u);
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(98u, 4u, 48u, 448u), 100u);
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(102u, 4u, 48u, 448u), 104u);

    // 越界请求钳到 [min, max]，绝不交给引擎去拒。
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(1u, 1u, 336u, 480u), 336u);
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(1'000'000u, 1u, 336u, 480u), 480u);
    // 病态请求（u32 上限）必须在 u64 域算完再钳，不能回绕成 0。
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(0xFFFFFFFFu, 1u, 336u, 480u), 480u);

    // 拿不到合法周期时返回 0：调用方据此退回"不请求周期"的旧式路径。
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(0u, 0u, 336u, 480u), 0u);
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(0u, 1u, 0u, 480u), 0u);
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(0u, 1u, 480u, 336u), 0u);
    // 区间内没有任何 fundamental 倍数（ceil(337/4)=85 > floor(339/4)=84）。
    EXPECT_EQ(aqua::audio::wasapi::choose_engine_period(0u, 4u, 337u, 339u), 0u);
}


// ---- MMCSS 两级降级链（wasapi_lifecycle.h 的 ScopedMmcssTask）----
// 本机无法自然构造 MMCSS 注册失败，所以注入假注册器覆盖三条路径。断言的重点是
// **调用顺序**：只断言最终 outcome() 的话，一个"只注册首选档、失败即放弃"的实现也
// 能全绿 —— 而"首选失败之后确实去试了降级档"才是这条链存在的全部意义。
namespace {

struct MmcssProbe {
    std::vector<std::wstring> acquired; // 依次收到的档名
    std::vector<HANDLE> released; // 收到 release 的句柄
    const wchar_t* reject = nullptr; // 该档名注册失败（nullptr = 不按名字拒）
    bool reject_all = false;
    DWORD reject_error = ERROR_NOT_FOUND; // 失败时应设置的 last error
};

MmcssProbe* g_mmcss_probe = nullptr;

HANDLE WINAPI probe_acquire(const wchar_t* task_name, DWORD* task_index) noexcept
{
    if (task_index != nullptr) {
        *task_index = 0;
    }
    g_mmcss_probe->acquired.emplace_back(task_name != nullptr ? task_name : L"");
    const bool rejected = g_mmcss_probe->reject_all
        || (g_mmcss_probe->reject != nullptr && task_name != nullptr
            && std::wcscmp(g_mmcss_probe->reject, task_name) == 0);
    if (rejected) {
        ::SetLastError(g_mmcss_probe->reject_error);
        return nullptr;
    }
    // 假句柄 = 第几次调用，便于断言"还的是自己拿到的那一个"。
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(g_mmcss_probe->acquired.size()));
}

BOOL WINAPI probe_release(HANDLE handle) noexcept
{
    g_mmcss_probe->released.push_back(handle);
    return TRUE;
}

const aqua::audio::wasapi::ScopedMmcssTask::Api kProbeApi { probe_acquire, probe_release };

// 假注册器是全局的，不能泄漏到别的用例。
class MmcssProbeScope {
public:
    explicit MmcssProbeScope(MmcssProbe& probe) noexcept { g_mmcss_probe = &probe; }
    ~MmcssProbeScope() { g_mmcss_probe = nullptr; }
    MmcssProbeScope(const MmcssProbeScope&) = delete;
    MmcssProbeScope& operator=(const MmcssProbeScope&) = delete;
};

HANDLE probe_handle(std::size_t nth_call) noexcept
{
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(nth_call));
}

} // namespace

TEST(WasapiMmcssLadderTest, PreferredTaskSucceedsAndFallbackIsNeverTried)
{
    MmcssProbe probe;
    MmcssProbeScope scope { probe };
    {
        const aqua::audio::wasapi::ScopedMmcssTask mmcss { L"Pro Audio", L"Playback", kProbeApi };
        EXPECT_EQ(static_cast<int>(mmcss.outcome()),
            static_cast<int>(aqua::audio::wasapi::ScopedMmcssTask::Outcome::Preferred));
        EXPECT_TRUE(mmcss.registered());
        EXPECT_STREQ(mmcss.task_name(), L"Pro Audio");
        EXPECT_EQ(mmcss.last_error(), 0ul);
    }
    ASSERT_EQ(probe.acquired.size(), 1u);
    EXPECT_EQ(probe.acquired[0], L"Pro Audio");
    ASSERT_EQ(probe.released.size(), 1u);
    EXPECT_EQ(probe.released[0], probe_handle(1));
}

TEST(WasapiMmcssLadderTest, FallsBackToTheSecondTaskInOrder)
{
    MmcssProbe probe;
    probe.reject = L"Pro Audio";
    MmcssProbeScope scope { probe };
    {
        const aqua::audio::wasapi::ScopedMmcssTask mmcss { L"Pro Audio", L"Playback", kProbeApi };
        EXPECT_EQ(static_cast<int>(mmcss.outcome()),
            static_cast<int>(aqua::audio::wasapi::ScopedMmcssTask::Outcome::Fallback));
        EXPECT_TRUE(mmcss.registered());
        EXPECT_STREQ(mmcss.task_name(), L"Playback");
    }
    ASSERT_EQ(probe.acquired.size(), 2u);
    EXPECT_EQ(probe.acquired[0], L"Pro Audio");
    EXPECT_EQ(probe.acquired[1], L"Playback");
    // 还的是**降级那次**拿到的句柄（第 2 次调用），不是首选失败时留下的空句柄。
    ASSERT_EQ(probe.released.size(), 1u);
    EXPECT_EQ(probe.released[0], probe_handle(2));
}

TEST(WasapiMmcssLadderTest, GivesUpWithoutMmcssWhenEveryTaskIsRejected)
{
    MmcssProbe probe;
    probe.reject_all = true;
    MmcssProbeScope scope { probe };
    {
        const aqua::audio::wasapi::ScopedMmcssTask mmcss { L"Pro Audio", L"Playback", kProbeApi };
        EXPECT_EQ(static_cast<int>(mmcss.outcome()),
            static_cast<int>(aqua::audio::wasapi::ScopedMmcssTask::Outcome::None));
        EXPECT_FALSE(mmcss.registered());
        EXPECT_EQ(mmcss.task_name(), nullptr);
        // 失败原因必须带得出来 —— 调用方的日志/排障全靠它。
        EXPECT_EQ(mmcss.last_error(), static_cast<unsigned long>(ERROR_NOT_FOUND));
    }
    ASSERT_EQ(probe.acquired.size(), 2u); // 两档都试过才放弃
    EXPECT_TRUE(probe.released.empty()); // 没有句柄可还
}

TEST(WasapiMmcssLadderTest, WithoutFallbackTheLadderStopsAtThePreferredTask)
{
    MmcssProbe probe;
    probe.reject = L"Pro Audio";
    MmcssProbeScope scope { probe };
    {
        const aqua::audio::wasapi::ScopedMmcssTask mmcss { L"Pro Audio", nullptr, kProbeApi };
        EXPECT_FALSE(mmcss.registered());
    }
    ASSERT_EQ(probe.acquired.size(), 1u); // 没有降级档就不该多试一次
}
