#include "aqua/audio/audio_format.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/playback/audio_playback.h"

#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <optional>
#include <string>
#include <vector>
#include <thread>

namespace {

class ScopedComObject final {
public:
    explicit ScopedComObject(IUnknown* value) noexcept : value_(value) {}
    ~ScopedComObject() {
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
            wide_id.data(), wide_length) <= 0) {
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
            reinterpret_cast<void**>(&raw_client))) || raw_client == nullptr) {
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
        if (::IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
            format->wBitsPerSample == 32) {
            encoding = aqua::audio::AudioEncoding::PCM_F32LE;
        } else if (::IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            switch (format->wBitsPerSample) {
            case 8: encoding = aqua::audio::AudioEncoding::PCM_U8; break;
            case 16: encoding = aqua::audio::AudioEncoding::PCM_S16LE; break;
            case 24: encoding = aqua::audio::AudioEncoding::PCM_S24LE; break;
            case 32: encoding = aqua::audio::AudioEncoding::PCM_S32LE; break;
            default: return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) {
        encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    } else if (format->wFormatTag == WAVE_FORMAT_PCM) {
        switch (format->wBitsPerSample) {
        case 8: encoding = aqua::audio::AudioEncoding::PCM_U8; break;
        case 16: encoding = aqua::audio::AudioEncoding::PCM_S16LE; break;
        case 24: encoding = aqua::audio::AudioEncoding::PCM_S24LE; break;
        case 32: encoding = aqua::audio::AudioEncoding::PCM_S32LE; break;
        default: return std::nullopt;
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

std::uint32_t playback_callback(void* user_data, std::span<std::byte> output) noexcept
{
    auto& stats = *static_cast<PlaybackStats*>(user_data);
    ++stats.callback_count;
    std::fill(output.begin(), output.end(), std::byte { 0 });
    return stats.frame_bytes == 0
        ? 0U
        : static_cast<std::uint32_t>(output.size() / stats.frame_bytes);
}

void playback_event_callback(void* user_data, aqua::audio::AudioError error) noexcept
{
    auto& stats = *static_cast<PlaybackStats*>(user_data);
    stats.last_error.store(error, std::memory_order_release);
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
        .device = std::nullopt,
        .format = *format,
        .frames_per_buffer = 0,
    };

    const auto result = playback->start(
        config,
        playback_callback,
        &stats,
        playback_event_callback,
        &stats);
    ASSERT_TRUE(result) << "start() failed";
    ASSERT_TRUE(playback->is_running());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    playback->stop();

    EXPECT_FALSE(playback->is_running());
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
        .device = devices.front().id,
        .format = *format,
        .frames_per_buffer = 0,
    };

    const auto result = playback->start(
        config,
        playback_callback,
        &stats,
        playback_event_callback,
        &stats);
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
        .device = std::nullopt,
        .format = {},
        .frames_per_buffer = 0,
    };

    PlaybackStats stats;
    const auto result = playback->start(config, playback_callback, &stats);
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
        .device = std::nullopt,
        .format = *format,
        .frames_per_buffer = 0,
    };
    PlaybackStats stats;
    ASSERT_TRUE(playback->start(config, playback_callback, &stats));

    const auto second = playback->start(config, playback_callback, &stats);
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
        .device = std::nullopt,
        .format = aqua::audio::AudioFormat {
            .encoding = aqua::audio::AudioEncoding::PCM_S16LE,
            .channels = 2,
            .sample_rate = 48000,
        },
        .frames_per_buffer = 0,
    };

    const auto result = playback->start(config, nullptr, nullptr);
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
        .device = aqua::audio::AudioDeviceId { "aqua/nonexistent/device/id" },
        .format = aqua::audio::AudioFormat {
            .encoding = aqua::audio::AudioEncoding::PCM_S16LE,
            .channels = 2,
            .sample_rate = 48000,
        },
        .frames_per_buffer = 0,
    };

    PlaybackStats stats;
    const auto result = playback->start(config, playback_callback, &stats);
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
        .device = aqua::audio::AudioDeviceId { "\xFF\xFE\xFD" },
        .format = aqua::audio::AudioFormat {
            .encoding = aqua::audio::AudioEncoding::PCM_S16LE,
            .channels = 2,
            .sample_rate = 48000,
        },
        .frames_per_buffer = 0,
    };

    PlaybackStats stats;
    const auto result = playback->start(config, playback_callback, &stats);
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
        .device = aqua::audio::AudioDeviceId { "aqua/nonexistent/device/id" },
        .format = *format,
        .frames_per_buffer = 0,
    };
    const auto bad = playback->start(bad_config, playback_callback, &stats);
    ASSERT_FALSE(bad);

    const aqua::audio::AudioPlaybackConfig good_config {
        .device = std::nullopt,
        .format = *format,
        .frames_per_buffer = 0,
    };
    const auto good = playback->start(good_config, playback_callback, &stats);
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

    // A huge requested buffer must be clamped to the engine's supported range
    // rather than being rejected or overflowing.
    const aqua::audio::AudioPlaybackConfig config {
        .device = std::nullopt,
        .format = *format,
        .frames_per_buffer = 0xFFFFFFFFu,
    };
    const auto result = playback->start(config, playback_callback, &stats);
    ASSERT_TRUE(result) << "start with huge frames_per_buffer failed";
    EXPECT_TRUE(playback->is_running());

    playback->stop();
    EXPECT_FALSE(playback->is_running());
}
