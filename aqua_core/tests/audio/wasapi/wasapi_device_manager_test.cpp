#include "aqua/audio/devices/audio_device_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using aqua::audio::AudioDevice;
using aqua::audio::AudioDeviceDirection;
using aqua::audio::AudioDeviceManager;
using aqua::audio::AudioError;

namespace {

    const char* to_string(AudioDeviceDirection direction)
    {
        switch (direction) {
        case AudioDeviceDirection::INPUT:
            return "INPUT";
        case AudioDeviceDirection::OUTPUT:
            return "OUTPUT";
        case AudioDeviceDirection::NONE:
            return "NONE";
        }
        return "UNKNOWN";
    }

    void print_device(const AudioDevice& device, std::size_t index)
    {
        std::cout
            << "  [" << index << "]\n"
            << "      direction : " << to_string(device.direction) << '\n'
            << "      default   : " << std::boolalpha << device.is_default << '\n'
            << "      name      : " << device.name << '\n'
            << "      id        : " << device.id.value() << '\n';
    }

    void print_devices(
        AudioDeviceManager& manager,
        AudioDeviceDirection direction)
    {
        const auto devices = manager.enumerate(direction);

        std::cout
            << "\n=== " << to_string(direction) << " devices ("
            << devices.size() << ") ===\n";

        if (devices.empty()) {
            std::cout << "  <none>\n";
        } else {
            for (std::size_t i = 0; i < devices.size(); ++i) {
                print_device(devices[i], i);
            }
        }

        const auto default_device = manager.default_device(direction);
        std::cout << "  default_device():\n";
        if (default_device) {
            print_device(*default_device, 0);
        } else {
            std::cout << "    <none>\n";
        }
    }

} // namespace

TEST(WasapiAudioDeviceManagerTest, PrintsAllDevices)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    std::cout << "========================================\n"
              << " WASAPI AUDIO DEVICE DUMP\n"
              << "========================================\n";

    print_devices(*manager, AudioDeviceDirection::INPUT);
    print_devices(*manager, AudioDeviceDirection::OUTPUT);

    std::cout << "\n=== resolve(default) ===\n";
    for (const auto direction : {
             AudioDeviceDirection::INPUT,
             AudioDeviceDirection::OUTPUT }) {
        const auto resolved = manager->resolve(direction, std::nullopt);

        std::cout << "  " << to_string(direction) << ": ";
        if (resolved) {
            std::cout << "success\n";
            print_device(*resolved, 0);
        } else {
            std::cout << "failed, error="
                      << static_cast<int>(resolved.error()) << '\n';
        }
    }

    std::cout << "========================================\n\n";
}

TEST(WasapiAudioDeviceManagerTest, EnumeratesInputAndOutputWithoutInvalidDirections)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    const auto inputs = manager->enumerate(AudioDeviceDirection::INPUT);
    const auto outputs = manager->enumerate(AudioDeviceDirection::OUTPUT);

    for (const AudioDevice& device : inputs) {
        EXPECT_EQ(device.direction, AudioDeviceDirection::INPUT);
        EXPECT_FALSE(device.id.empty());
        EXPECT_FALSE(device.name.empty());
    }

    for (const AudioDevice& device : outputs) {
        EXPECT_EQ(device.direction, AudioDeviceDirection::OUTPUT);
        EXPECT_FALSE(device.id.empty());
        EXPECT_FALSE(device.name.empty());
    }
}

TEST(WasapiAudioDeviceManagerTest, DefaultDeviceMatchesEnumeratedDevice)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    for (const auto direction : { AudioDeviceDirection::INPUT, AudioDeviceDirection::OUTPUT }) {
        const auto devices = manager->enumerate(direction);
        const auto default_device = manager->default_device(direction);

        if (!default_device) {
            GTEST_SKIP() << "No default WASAPI device is available for direction "
                         << (direction == AudioDeviceDirection::INPUT ? "INPUT" : "OUTPUT");
        }

        EXPECT_TRUE(default_device->is_default);
        EXPECT_EQ(default_device->direction, direction);
        EXPECT_FALSE(default_device->id.empty());

        const auto it = std::find_if(devices.begin(), devices.end(), [&](const AudioDevice& device) {
            return device.id == default_device->id;
        });
        ASSERT_NE(it, devices.end());
        EXPECT_TRUE(it->is_default);
    }
}

TEST(WasapiAudioDeviceManagerTest, ResolveDefaultReturnsDefaultDevice)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    for (const auto direction : { AudioDeviceDirection::INPUT, AudioDeviceDirection::OUTPUT }) {
        const auto default_device = manager->default_device(direction);
        if (!default_device) {
            GTEST_SKIP() << "No default WASAPI device is available for this direction";
        }

        const auto resolved = manager->resolve(direction, std::nullopt);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(resolved->id, default_device->id);
        EXPECT_EQ(resolved->direction, direction);
        EXPECT_TRUE(resolved->is_default);
    }
}

TEST(WasapiAudioDeviceManagerTest, ResolveSpecifiedDevicePreservesIdentityAndDirection)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    for (const auto direction : { AudioDeviceDirection::INPUT, AudioDeviceDirection::OUTPUT }) {
        const auto devices = manager->enumerate(direction);
        if (devices.empty()) {
            GTEST_SKIP() << "No WASAPI device is available for this direction";
        }

        const auto resolved = manager->resolve(direction, devices.front().id);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(resolved->id, devices.front().id);
        EXPECT_EQ(resolved->direction, direction);
    }
}

TEST(WasapiAudioDeviceManagerTest, ResolvingDeviceWithWrongDirectionFails)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    const auto inputs = manager->enumerate(AudioDeviceDirection::INPUT);
    const auto outputs = manager->enumerate(AudioDeviceDirection::OUTPUT);

    if (!inputs.empty() && !outputs.empty()) {
        const auto wrong_input = manager->resolve(AudioDeviceDirection::OUTPUT, inputs.front().id);
        ASSERT_FALSE(wrong_input.has_value());
        EXPECT_EQ(wrong_input.error(), AudioError::DeviceNotFound);

        const auto wrong_output = manager->resolve(AudioDeviceDirection::INPUT, outputs.front().id);
        ASSERT_FALSE(wrong_output.has_value());
        EXPECT_EQ(wrong_output.error(), AudioError::DeviceNotFound);
    } else {
        GTEST_SKIP() << "Both input and output WASAPI devices are required for this test";
    }
}

TEST(WasapiAudioDeviceManagerTest, DefaultFormatIsAvailableForSelectedDefaultEndpoint)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    for (const auto direction : { AudioDeviceDirection::INPUT, AudioDeviceDirection::OUTPUT }) {
        const auto format = manager->default_format(direction, std::nullopt);
        if (!format) {
            GTEST_SKIP() << "No usable WASAPI default format for direction "
                         << to_string(direction) << ": error=" << static_cast<int>(format.error());
        }
        EXPECT_TRUE(format->is_valid());
        EXPECT_GT(format->frame_bytes(), 0u);
    }
}

TEST(WasapiAudioDeviceManagerTest, ResolvingDefaultDeviceByIdIsMarkedDefault)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    for (const auto direction : { AudioDeviceDirection::INPUT, AudioDeviceDirection::OUTPUT }) {
        const auto default_device = manager->default_device(direction);
        if (!default_device) {
            GTEST_SKIP() << "No default WASAPI device is available for this direction";
        }

        const auto resolved = manager->resolve(direction, default_device->id);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(resolved->id, default_device->id);
        EXPECT_TRUE(resolved->is_default);
    }
}

TEST(WasapiAudioDeviceManagerTest, EmptySpecifiedDeviceIsInvalidArgument)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    const auto result = manager->resolve(
        AudioDeviceDirection::INPUT,
        aqua::audio::AudioDeviceId { });

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AudioError::InvalidArgument);
}

TEST(WasapiAudioDeviceManagerTest, ResolvingUnknownDeviceFails)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    const auto result = manager->resolve(
        AudioDeviceDirection::INPUT,
        aqua::audio::AudioDeviceId { "aqua/nonexistent/device/id" });

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AudioError::DeviceNotFound);
}

TEST(WasapiAudioDeviceManagerTest, NoneDirectionIsRejected)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    EXPECT_TRUE(manager->enumerate(AudioDeviceDirection::NONE).empty());
    EXPECT_FALSE(manager->default_device(AudioDeviceDirection::NONE).has_value());

    const auto result = manager->resolve(AudioDeviceDirection::NONE, std::nullopt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AudioError::InvalidArgument);
}

TEST(WasapiAudioDeviceManagerTest, ResolvingInvalidUtf8DeviceIdIsRejected)
{
    auto manager = aqua::audio::create_device_manager();
    ASSERT_NE(manager, nullptr);

    // 非空但不是合法 UTF-8 的 id 无法转成宽字符 endpoint id，
    // 因此 resolve() 必须返回 InvalidArgument。
    const auto result = manager->resolve(
        AudioDeviceDirection::INPUT,
        aqua::audio::AudioDeviceId { "\xFF\xFE\xFD" });

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AudioError::InvalidArgument);
}

} // namespace