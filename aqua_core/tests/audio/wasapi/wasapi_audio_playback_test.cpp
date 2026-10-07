#include "aqua/audio/audio_format.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/playback/audio_playback.h"
#include "audio/public/wasapi/wasapi_engine_period.h"

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
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
