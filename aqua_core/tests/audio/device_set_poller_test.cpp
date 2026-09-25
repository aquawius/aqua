// DeviceSetPoller 单测：轮询去抖与"无信息"门控。
//
// 覆盖两条来自实现事实的约束（见 device_set_poller.h 头注释）：
//   1. 单次采样不可信 —— 变化必须连续两次一致才上报；
//   2. 有些平台没有可枚举集合 —— 空 id / 空集合 / 抛异常一律按无信息处理，
//      既不上报也不冲掉已有基线（Android 的合成条目就是这一类）。

#include "aqua/audio/devices/device_set_poller.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace aqua::audio {
namespace {

    class StubDeviceManager final : public AudioDeviceManager {
    public:
        void set_ids(std::vector<std::string> ids)
        {
            devices_.clear();
            for (auto& id : ids) {
                AudioDevice device;
                device.id = AudioDeviceId(std::move(id));
                device.direction = AudioDeviceDirection::OUTPUT;
                devices_.push_back(std::move(device));
            }
        }

        // 空 id 的合成条目：Android 的 enumerate 只返回这一条。
        void set_synthetic_system_default()
        {
            devices_.clear();
            AudioDevice device;
            device.id = AudioDeviceId { }; // 空 id = 系统默认
            device.name = "System Default Output";
            device.direction = AudioDeviceDirection::OUTPUT;
            device.is_default = true;
            devices_.push_back(std::move(device));
        }

        void set_throws(bool value) noexcept { throws_ = value; }

        [[nodiscard]] std::vector<AudioDevice>
        enumerate(AudioDeviceDirection) const override
        {
            if (throws_) {
                throw std::runtime_error("enumeration unavailable");
            }
            return devices_;
        }

        [[nodiscard]] std::optional<AudioDevice>
        default_device(AudioDeviceDirection) const override
        {
            return std::nullopt;
        }

        [[nodiscard]] std::expected<AudioFormat, AudioError>
        default_format(AudioDeviceDirection,
            const std::optional<AudioDeviceId>&) const override
        {
            return std::unexpected(AudioError::NotSupported);
        }

        [[nodiscard]] std::expected<AudioDevice, AudioError>
        resolve(AudioDeviceDirection,
            const std::optional<AudioDeviceId>&) const override
        {
            return std::unexpected(AudioError::NotSupported);
        }

    private:
        std::vector<AudioDevice> devices_;
        bool throws_ = false;
    };

    std::vector<std::string> ids(const std::vector<AudioDeviceId>& in)
    {
        std::vector<std::string> out;
        out.reserve(in.size());
        for (const auto& id : in) {
            out.push_back(id.value());
        }
        return out;
    }

    using StrVec = std::vector<std::string>;

    // ---- 无信息门控（Android / 枚举失败）----

    TEST(DeviceSetPollerTest, SyntheticEmptyIdIsNotInformation)
    {
        StubDeviceManager devices;
        devices.set_synthetic_system_default();
        DeviceSetPoller poller(devices, AudioDeviceDirection::OUTPUT);

        // 反复轮询也永不上报：把合成条目当真相等于每个 tick 都判定活跃设备缺席。
        for (int i = 0; i < 5; ++i) {
            EXPECT_FALSE(poller.poll().has_value()) << "iteration " << i;
        }
    }

    TEST(DeviceSetPollerTest, EmptyEnumerationIsValidEmptySet)
    {
        // 原始空列表 = WASAPI 真零设备（有效空集），不是无信息：
        // 去抖后上报基线 present=[]，使全拔场景能检出 active 消失。
        // Android 合成条目（含空 id）仍是无信息，见上一个测试。
        StubDeviceManager devices;
        DeviceSetPoller poller(devices, AudioDeviceDirection::OUTPUT);

        EXPECT_FALSE(poller.poll().has_value()); // 去抖第 1 次
        const auto change = poller.poll();
        ASSERT_TRUE(change.has_value()); // 基线 present=[]
        EXPECT_TRUE(change->present.empty());
        EXPECT_TRUE(change->added.empty());
        EXPECT_TRUE(change->removed.empty());
    }

    TEST(DeviceSetPollerTest, RemovalToEmptyIsReported)
    {
        StubDeviceManager devices;
        devices.set_ids({ "a" });
        DeviceSetPoller poller(devices, AudioDeviceDirection::OUTPUT);
        ASSERT_FALSE(poller.poll().has_value());
        ASSERT_TRUE(poller.poll().has_value()); // 基线 [a]

        devices.set_ids({ }); // 全拔
        EXPECT_FALSE(poller.poll().has_value()); // 去抖第 1 次
        const auto change = poller.poll();
        ASSERT_TRUE(change.has_value());
        EXPECT_TRUE(change->present.empty());
        EXPECT_EQ(ids(change->removed), (StrVec { "a" }));
    }

    TEST(DeviceSetPollerTest, EnumerationThrowingIsNotInformation)
    {
        StubDeviceManager devices;
        devices.set_ids({ "a" });
        DeviceSetPoller poller(devices, AudioDeviceDirection::OUTPUT);
        ASSERT_FALSE(poller.poll().has_value()); // 去抖第 1 次
        ASSERT_TRUE(poller.poll().has_value()); // 基线上报

        devices.set_throws(true);
        EXPECT_FALSE(poller.poll().has_value());
        EXPECT_FALSE(poller.poll().has_value());

        // 异常不得冲掉已上报的基线：恢复后同一集合仍是"无变化"。
        devices.set_throws(false);
        EXPECT_FALSE(poller.poll().has_value());
        EXPECT_FALSE(poller.poll().has_value());
    }

    TEST(DeviceSetPollerTest, NoInformationDoesNotErasePendingDebounce)
    {
        StubDeviceManager devices;
        devices.set_ids({ "a", "b" });
        DeviceSetPoller poller(devices, AudioDeviceDirection::OUTPUT);
        ASSERT_FALSE(poller.poll().has_value()); // 采样 [a,b]，hits=1

        // 中间插入一次"无信息"采样：不得重置去抖计数（否则 Android 上
        // 推送与轮询并存时永远确认不了任何变化）。
        devices.set_synthetic_system_default();
        EXPECT_FALSE(poller.poll().has_value());

        devices.set_ids({ "a", "b" });
        // 无信息的那次采样没有重置去抖计数，所以这就是第 2 次一致采样：
        // 直接上报基线。（若"无信息"会清状态，这里将退回 hits=1 而不上报。）
        const auto change = poller.poll();
        ASSERT_TRUE(change.has_value());
        EXPECT_EQ(ids(change->present), (StrVec { "a", "b" }));
    }

    // ---- 去抖 ----

    TEST(DeviceSetPollerTest, FirstConfirmedSampleIsBaselineWithoutDiff)
    {
        StubDeviceManager devices;
        devices.set_ids({ "a", "b" });
        DeviceSetPoller poller(devices, AudioDeviceDirection::OUTPUT);

        EXPECT_FALSE(poller.poll().has_value()); // 首次见到该集合：只开始去抖
        const auto change = poller.poll();
        ASSERT_TRUE(change.has_value());
        EXPECT_EQ(ids(change->present), (StrVec { "a", "b" }));
        // 基线不是"新增设备"：added/removed 皆空。
        EXPECT_TRUE(change->added.empty());
        EXPECT_TRUE(change->removed.empty());
    }

    TEST(DeviceSetPollerTest, BaselineIsReportedOnlyOnce)
    {
        StubDeviceManager devices;
        devices.set_ids({ "a" });
        DeviceSetPoller poller(devices, AudioDeviceDirection::OUTPUT);

        EXPECT_FALSE(poller.poll().has_value());
        EXPECT_TRUE(poller.poll().has_value());
        // 集合不变：后续 tick 全部安静（否则每 500ms 一次假变化）。
        for (int i = 0; i < 4; ++i) {
            EXPECT_FALSE(poller.poll().has_value()) << "iteration " << i;
        }
    }

    TEST(DeviceSetPollerTest, RemovalNeedsTwoConsistentSamples)
    {
        StubDeviceManager devices;
        devices.set_ids({ "a", "b" });
        DeviceSetPoller poller(devices, AudioDeviceDirection::OUTPUT);
        ASSERT_FALSE(poller.poll().has_value());
        ASSERT_TRUE(poller.poll().has_value()); // 基线 [a,b]

        devices.set_ids({ "a" }); // b 被拔出
        EXPECT_FALSE(poller.poll().has_value()); // 单次采样不上报
        const auto change = poller.poll();
        ASSERT_TRUE(change.has_value());
        EXPECT_EQ(ids(change->present), (StrVec { "a" }));
        EXPECT_TRUE(change->added.empty());
        EXPECT_EQ(ids(change->removed), (StrVec { "b" }));
    }

    TEST(DeviceSetPollerTest, TransientFlapWithinDebounceIsNeverReported)
    {
        StubDeviceManager devices;
        devices.set_ids({ "a", "b" });
        DeviceSetPoller poller(devices, AudioDeviceDirection::OUTPUT);
        ASSERT_FALSE(poller.poll().has_value());
        ASSERT_TRUE(poller.poll().has_value()); // 基线 [a,b]

        // WASAPI 在设备转换期可能瞬时枚举不全：只被看到一次的"缺席"不算数。
        devices.set_ids({ "a" });
        EXPECT_FALSE(poller.poll().has_value());
        devices.set_ids({ "a", "b" }); // 下一次采样已恢复
        for (int i = 0; i < 3; ++i) {
            EXPECT_FALSE(poller.poll().has_value()) << "iteration " << i;
        }
    }

    TEST(DeviceSetPollerTest, AddedAndRemovedComputedAgainstLastReport)
    {
        StubDeviceManager devices;
        devices.set_ids({ "a", "b" });
        DeviceSetPoller poller(devices, AudioDeviceDirection::OUTPUT);
        ASSERT_FALSE(poller.poll().has_value());
        ASSERT_TRUE(poller.poll().has_value()); // 基线 [a,b]

        devices.set_ids({ "b", "c" }); // a 拔出、c 插入（蓝牙连接风暴的典型形态）
        ASSERT_FALSE(poller.poll().has_value());
        const auto change = poller.poll();
        ASSERT_TRUE(change.has_value());
        EXPECT_EQ(ids(change->present), (StrVec { "b", "c" }));
        EXPECT_EQ(ids(change->added), (StrVec { "c" }));
        EXPECT_EQ(ids(change->removed), (StrVec { "a" }));
    }

    TEST(DeviceSetPollerTest, EnumerationOrderIsNotAChange)
    {
        StubDeviceManager devices;
        devices.set_ids({ "a", "b" });
        DeviceSetPoller poller(devices, AudioDeviceDirection::OUTPUT);
        ASSERT_FALSE(poller.poll().has_value());
        ASSERT_TRUE(poller.poll().has_value());

        // 枚举顺序不保证稳定：只有集合内容变化才算插拔。
        devices.set_ids({ "b", "a" });
        EXPECT_FALSE(poller.poll().has_value());
        EXPECT_FALSE(poller.poll().has_value());
    }

    TEST(DeviceSetPollerTest, ResetRestartsDebounceAndBaseline)
    {
        StubDeviceManager devices;
        devices.set_ids({ "a", "b" });
        DeviceSetPoller poller(devices, AudioDeviceDirection::OUTPUT);
        ASSERT_FALSE(poller.poll().has_value());
        ASSERT_TRUE(poller.poll().has_value());

        poller.reset(); // 新会话
        EXPECT_FALSE(poller.poll().has_value()); // 重新去抖
        const auto change = poller.poll();
        ASSERT_TRUE(change.has_value());
        // 重新建立的是基线，不是相对旧会话的差集。
        EXPECT_TRUE(change->added.empty());
        EXPECT_TRUE(change->removed.empty());
        EXPECT_EQ(ids(change->present), (StrVec { "a", "b" }));
    }

    // ---- 诊断格式化 ----

    TEST(DeviceSetPollerTest, FormatDeviceIdsRendersBrackets)
    {
        EXPECT_EQ(format_device_ids({ }), "[]");
        EXPECT_EQ(format_device_ids({ AudioDeviceId("a") }), "[a]");
        EXPECT_EQ(format_device_ids({ AudioDeviceId("a"), AudioDeviceId("b") }), "[a, b]");
    }

} // namespace
} // namespace aqua::audio
