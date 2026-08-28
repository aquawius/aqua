#include "aqua/audio/capture/audio_capture.h"
#include "aqua/audio/capture/audio_capture_config.h"
#include "aqua/audio/devices/audio_device_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <thread>

namespace {

using namespace std::chrono_literals;
using aqua::audio::AudioCaptureConfig;
using aqua::audio::AudioCaptureInfo;
using aqua::audio::AudioCaptureSource;
using aqua::audio::AudioDeviceDirection;
using aqua::audio::AudioError;
using aqua::audio::AudioBlock;

struct CaptureProbe {
    std::atomic<std::uint64_t> callbacks { 0 };
    std::atomic<std::uint64_t> bytes { 0 };
    std::atomic<bool> malformed { false };
    std::atomic<AudioError> runtime_error { AudioError::None };
};

auto make_capture_cb(CaptureProbe& probe)
{
    return [&probe](const AudioBlock& block) noexcept {
        probe.callbacks.fetch_add(1, std::memory_order_relaxed);
        probe.bytes.fetch_add(block.data.size(), std::memory_order_relaxed);

        if (block.data.empty()) {
            probe.malformed.store(true, std::memory_order_release);
        }
    };
}

auto make_event_cb(CaptureProbe& probe)
{
    return [&probe](AudioError error) noexcept {
        probe.runtime_error.store(error, std::memory_order_release);
    };
}

bool wait_for_first_frame(const CaptureProbe& probe, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (probe.callbacks.load(std::memory_order_acquire) > 0) {
            return true;
        }
        if (probe.runtime_error.load(std::memory_order_acquire) != AudioError::None) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return probe.callbacks.load(std::memory_order_acquire) > 0;
}

void print_capture_info(const AudioCaptureInfo& info)
{
    std::cout
        << "    format.encoding      : " << static_cast<int>(info.format.encoding) << '\n'
        << "    format.channels      : " << info.format.channels << '\n'
        << "    format.sample_rate   : " << info.format.sample_rate << '\n'
        << "    format.frame_bytes   : " << info.format.frame_bytes() << '\n'
        << "    frames_per_buffer    : " << info.frames_per_buffer << '\n';
}

TEST(WasapiAudioCaptureTest, DefaultInputStartsAndReportsActualFormat)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    const auto default_device = manager->default_device(AudioDeviceDirection::INPUT);
    if (!default_device) {
        GTEST_SKIP() << "No default WASAPI input device is available";
    }

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    AudioCaptureConfig config;
    config.source = AudioCaptureSource::INPUT_DEVICE;
    config.device = std::nullopt;
    config.format = std::nullopt;

    CaptureProbe probe;
    const auto result = capture->start(config, make_capture_cb(probe), make_event_cb(probe));
    ASSERT_TRUE(result.has_value())
        << "start failed, error=" << static_cast<int>(result.error());

    std::cout << "\n=== WASAPI default INPUT capture ===\n"
              << "  device.name           : " << default_device->name << '\n'
              << "  device.id             : " << default_device->id.value() << '\n';
    print_capture_info(capture->info());

    EXPECT_TRUE(capture->info().format.is_valid());
    EXPECT_GT(capture->info().frames_per_buffer, 0u);
    EXPECT_TRUE(capture->is_running());

    const bool received = wait_for_first_frame(probe, 2s);
    capture->stop();

    ASSERT_TRUE(received) << "did not receive an audio frame within 2s";
    EXPECT_FALSE(probe.malformed.load(std::memory_order_acquire));
    EXPECT_EQ(probe.runtime_error.load(std::memory_order_acquire), AudioError::None);
    EXPECT_GT(probe.bytes.load(std::memory_order_acquire), 0u);
    EXPECT_FALSE(capture->is_running());
}

TEST(WasapiAudioCaptureTest, SpecifiedInputStartsAndUsesSelectedDevice)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    const auto devices = manager->enumerate(AudioDeviceDirection::INPUT);
    if (devices.empty()) {
        GTEST_SKIP() << "No WASAPI input device is available";
    }

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    AudioCaptureConfig config;
    config.source = AudioCaptureSource::INPUT_DEVICE;
    config.device = devices.front().id;
    config.format = std::nullopt;

    CaptureProbe probe;
    const auto result = capture->start(config, make_capture_cb(probe), make_event_cb(probe));
    ASSERT_TRUE(result.has_value())
        << "start failed for device '" << devices.front().name
        << "', error=" << static_cast<int>(result.error());

    std::cout << "\n=== WASAPI specified INPUT capture ===\n"
              << "  device.name           : " << devices.front().name << '\n'
              << "  device.id             : " << devices.front().id.value() << '\n';
    print_capture_info(capture->info());

    EXPECT_TRUE(capture->info().format.is_valid());
    EXPECT_TRUE(capture->is_running());

    const bool received = wait_for_first_frame(probe, 2s);
    capture->stop();

    ASSERT_TRUE(received) << "did not receive an audio frame within 2s";
    EXPECT_FALSE(probe.malformed.load(std::memory_order_acquire));
    EXPECT_GT(probe.bytes.load(std::memory_order_acquire), 0u);
}

TEST(WasapiAudioCaptureTest, RequestedNativeFormatIsAccepted)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    const auto default_device = manager->default_device(AudioDeviceDirection::INPUT);
    if (!default_device) {
        GTEST_SKIP() << "No default WASAPI input device is available";
    }

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    CaptureProbe probe;
    AudioCaptureConfig probe_config;
    probe_config.source = AudioCaptureSource::INPUT_DEVICE;
    const auto probe_result = capture->start(
        probe_config, make_capture_cb(probe), make_event_cb(probe));
    ASSERT_TRUE(probe_result.has_value())
        << "initial stream failed, error=" << static_cast<int>(probe_result.error());

    const auto actual_format = capture->info().format;
    capture->stop();

    AudioCaptureConfig requested_config;
    requested_config.source = AudioCaptureSource::INPUT_DEVICE;
    requested_config.format = actual_format;

    const auto requested_result = capture->start(
        requested_config, make_capture_cb(probe), make_event_cb(probe));
    ASSERT_TRUE(requested_result.has_value())
        << "native shared-mode format was rejected, error="
        << static_cast<int>(requested_result.error());

    EXPECT_EQ(capture->info().format, actual_format);
    capture->stop();
}

TEST(WasapiAudioCaptureTest, StartWhileRunningIsRejected)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);
    if (!manager->default_device(AudioDeviceDirection::INPUT)) {
        GTEST_SKIP() << "No default WASAPI input device is available";
    }

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    CaptureProbe probe;
    const AudioCaptureConfig config {};
    ASSERT_TRUE(capture->start(config, make_capture_cb(probe)));

    const auto second = capture->start(config, make_capture_cb(probe));
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), AudioError::AlreadyRunning);

    capture->stop();
}

TEST(WasapiAudioCaptureTest, CanStopAndStartAgain)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);
    if (!manager->default_device(AudioDeviceDirection::INPUT)) {
        GTEST_SKIP() << "No default WASAPI input device is available";
    }

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    CaptureProbe first_probe;
    const AudioCaptureConfig config {};
    ASSERT_TRUE(capture->start(config, make_capture_cb(first_probe)));
    ASSERT_TRUE(wait_for_first_frame(first_probe, 2s));
    capture->stop();
    EXPECT_FALSE(capture->is_running());

    CaptureProbe second_probe;
    ASSERT_TRUE(capture->start(config, make_capture_cb(second_probe)));
    ASSERT_TRUE(wait_for_first_frame(second_probe, 2s));
    capture->stop();
    EXPECT_FALSE(capture->is_running());
}

TEST(WasapiAudioCaptureTest, OutputLoopbackStartsAndReceivesFrames)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    const auto default_device = manager->default_device(AudioDeviceDirection::OUTPUT);
    if (!default_device) {
        GTEST_SKIP() << "No default WASAPI output device is available";
    }

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    AudioCaptureConfig config;
    config.source = AudioCaptureSource::OUTPUT_LOOPBACK;
    config.device = std::nullopt;
    config.format = std::nullopt;

    CaptureProbe probe;
    const auto result = capture->start(config, make_capture_cb(probe), make_event_cb(probe));
    ASSERT_TRUE(result.has_value())
        << "loopback start failed, error=" << static_cast<int>(result.error());

    EXPECT_TRUE(capture->info().format.is_valid());
    EXPECT_GT(capture->info().frames_per_buffer, 0u);
    EXPECT_TRUE(capture->is_running());

    const bool received = wait_for_first_frame(probe, 2s);
    capture->stop();

    ASSERT_TRUE(received) << "did not receive a loopback audio frame within 2s";
    EXPECT_FALSE(probe.malformed.load(std::memory_order_acquire));
    EXPECT_GT(probe.bytes.load(std::memory_order_acquire), 0u);
    EXPECT_FALSE(capture->is_running());
}

TEST(WasapiAudioCaptureTest, StartWithNullCallbackIsRejected)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    const AudioCaptureConfig config {};
    const auto result = capture->start(config, nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AudioError::InvalidArgument);
    EXPECT_FALSE(capture->is_running());
}

TEST(WasapiAudioCaptureTest, StartWithInvalidSourceIsRejected)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    AudioCaptureConfig config;
    config.source = static_cast<AudioCaptureSource>(0xFF);

    CaptureProbe probe;
    const auto result = capture->start(config, make_capture_cb(probe));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AudioError::InvalidArgument);
    EXPECT_FALSE(capture->is_running());
}

TEST(WasapiAudioCaptureTest, StartWithInvalidFormatIsRejected)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    AudioCaptureConfig config;
    config.format = aqua::audio::AudioFormat {}; // 默认构造的 format 非法

    CaptureProbe probe;
    const auto result = capture->start(config, make_capture_cb(probe));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AudioError::InvalidArgument);
    EXPECT_FALSE(capture->is_running());
}

TEST(WasapiAudioCaptureTest, StartWithUnknownDeviceFails)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    AudioCaptureConfig config;
    config.source = AudioCaptureSource::INPUT_DEVICE;
    config.device = aqua::audio::AudioDeviceId { "aqua/nonexistent/device/id" };

    CaptureProbe probe;
    const auto result = capture->start(config, make_capture_cb(probe));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AudioError::DeviceNotFound);
    EXPECT_FALSE(capture->is_running());
}

TEST(WasapiAudioCaptureTest, StartWithWrongDirectionDeviceFails)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    const auto inputs = manager->enumerate(AudioDeviceDirection::INPUT);
    const auto outputs = manager->enumerate(AudioDeviceDirection::OUTPUT);
    if (inputs.empty() || outputs.empty()) {
        GTEST_SKIP() << "Both input and output WASAPI devices are required for this test";
    }

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    CaptureProbe probe;

    {
        AudioCaptureConfig config;
        config.source = AudioCaptureSource::INPUT_DEVICE;
        config.device = outputs.front().id;
        const auto result = capture->start(config, make_capture_cb(probe));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), AudioError::DeviceNotFound);
    }

    {
        AudioCaptureConfig config;
        config.source = AudioCaptureSource::OUTPUT_LOOPBACK;
        config.device = inputs.front().id;
        const auto result = capture->start(config, make_capture_cb(probe));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), AudioError::DeviceNotFound);
    }
}

TEST(WasapiAudioCaptureTest, StopBeforeStartIsNoOp)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    EXPECT_FALSE(capture->is_running());
    capture->stop();
    EXPECT_FALSE(capture->is_running());
    capture->stop();
    EXPECT_FALSE(capture->is_running());
}

TEST(WasapiAudioCaptureTest, InfoBeforeStartIsDefault)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    const auto& info = capture->info();
    EXPECT_FALSE(info.format.is_valid());
    EXPECT_EQ(info.frames_per_buffer, 0u);
}

TEST(WasapiAudioCaptureTest, CanStartAfterFailedStart)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);
    if (!manager->default_device(AudioDeviceDirection::INPUT)) {
        GTEST_SKIP() << "No default WASAPI input device is available";
    }

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    CaptureProbe probe;

    AudioCaptureConfig bad_config;
    bad_config.source = AudioCaptureSource::INPUT_DEVICE;
    bad_config.device = aqua::audio::AudioDeviceId { "aqua/nonexistent/device/id" };
    const auto bad = capture->start(bad_config, make_capture_cb(probe));
    ASSERT_FALSE(bad.has_value());

    AudioCaptureConfig good_config;
    good_config.source = AudioCaptureSource::INPUT_DEVICE;
    const auto good = capture->start(good_config, make_capture_cb(probe));
    ASSERT_TRUE(good.has_value())
        << "start after failed start failed, error=" << static_cast<int>(good.error());

    EXPECT_TRUE(capture->is_running());
    capture->stop();
    EXPECT_FALSE(capture->is_running());
}

TEST(WasapiAudioCaptureTest, StartWithInvalidUtf8DeviceIdIsRejected)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    AudioCaptureConfig config;
    config.source = AudioCaptureSource::INPUT_DEVICE;
    config.device = aqua::audio::AudioDeviceId { "\xFF\xFE\xFD" };

    CaptureProbe probe;
    const auto result = capture->start(config, make_capture_cb(probe));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AudioError::InvalidArgument);
    EXPECT_FALSE(capture->is_running());
}

TEST(WasapiAudioCaptureTest, OutputLoopbackWithSpecifiedDeviceStarts)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    const auto outputs = manager->enumerate(AudioDeviceDirection::OUTPUT);
    if (outputs.empty()) {
        GTEST_SKIP() << "No WASAPI output device is available";
    }

    auto capture = aqua::audio::create_capture(*manager);
    ASSERT_NE(capture, nullptr);

    AudioCaptureConfig config;
    config.source = AudioCaptureSource::OUTPUT_LOOPBACK;
    config.device = outputs.front().id;
    config.format = std::nullopt;

    CaptureProbe probe;
    const auto result = capture->start(config, make_capture_cb(probe));
    ASSERT_TRUE(result.has_value())
        << "loopback start failed, error=" << static_cast<int>(result.error());

    EXPECT_TRUE(capture->info().format.is_valid());
    EXPECT_GT(capture->info().frames_per_buffer, 0u);
    EXPECT_TRUE(capture->is_running());

    capture->stop();
    EXPECT_FALSE(capture->is_running());
}

} // namespace
