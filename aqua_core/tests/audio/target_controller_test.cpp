// Phase 1 TargetController 单测：纯计算语义 + estimator 联动轨迹验收。
// 验收（细则 §13）：clean→下降、jitter 升→上升、恢复→缓慢下降、不振荡。

#include "aqua/audio/buffer/jitter_estimator.h"
#include "aqua/audio/buffer/target_controller.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using aqua::audio::JitterEstimator;
using aqua::audio::TargetController;
using aqua::audio::TargetControllerParams;

constexpr std::int64_t kPacketNs = 10'000'000;

TargetControllerParams make_params()
{
    TargetControllerParams params;
    params.capacity_slots = 30;
    params.packet_ms = 10.0;
    return params; // min=2 initial=4 k=2 fall=1/s deadband=1
}

TEST(TargetControllerTest, CleanNetworkFallsToMinImmediately)
{
    TargetController controller(make_params());
    EXPECT_EQ(controller.current(), 4u);
    // 零抖动：期望 = ceil(0) → min=2，首拍无时间基全额跟进。
    EXPECT_EQ(controller.update(0.0, 0.0, 1'000'000'000), 2u);
    EXPECT_EQ(controller.update(0.0, 0.0, 1'000'000'000 + kPacketNs), 2u);
}

TEST(TargetControllerTest, JitterSpikeRisesFast)
{
    TargetController controller(make_params());
    controller.update(0.0, 0.0, 1'000'000'000);
    ASSERT_EQ(controller.current(), 2u);
    // J=30ms：期望 = ceil(2×30/10) = 6，超死区立即跟进（单拍）。
    EXPECT_EQ(controller.update(0.0, 30.0, 1'000'000'000 + kPacketNs), 6u);
}

TEST(TargetControllerTest, RecoveryFallsAtLimitedRate)
{
    TargetController controller(make_params());
    std::int64_t now = 1'000'000'000;
    controller.update(0.0, 30.0, now); // 首拍全额 → max(2, 6)=6
    ASSERT_EQ(controller.current(), 6u);
    // 恢复干净：10ms 包间隔下 1 格/秒 → 前 50 包（0.5s）一格不动。
    for (int i = 0; i < 50; ++i) {
        now += kPacketNs;
        EXPECT_EQ(controller.update(0.0, 0.0, now), 6u) << "packet " << i;
    }
    // 再 0.6s（60 包）降 1 格到 5。
    for (int i = 0; i < 60; ++i) {
        now += kPacketNs;
        controller.update(0.0, 0.0, now);
    }
    EXPECT_EQ(controller.current(), 5u);
    // 长期恢复单调不增，最终回到 min。
    std::uint32_t last = controller.current();
    for (int i = 0; i < 500; ++i) {
        now += kPacketNs;
        const auto current = controller.update(0.0, 0.0, now);
        EXPECT_LE(current, last);
        last = current;
    }
    EXPECT_EQ(controller.current(), 2u);
}

TEST(TargetControllerTest, DeadbandSuppressesSingleSlotChatter)
{
    TargetController controller(make_params());
    std::int64_t now = 1'000'000'000;
    controller.update(0.0, 0.0, now);
    ASSERT_EQ(controller.current(), 2u);
    // 期望在死区内（2±1）波动：target 纹丝不动。
    for (int i = 0; i < 100; ++i) {
        now += kPacketNs;
        // J 在 0~15ms 间摆动 → 期望 2~5，但死区+限速下首拍后应稳定。
        const double jitter = (i % 2 == 0) ? 0.0 : 5.0;
        controller.update(0.0, jitter, now);
    }
    // J=5 期望 ceil(2×5/10)=1→min=2，与当前差 0，死区内，不动。
    EXPECT_EQ(controller.current(), 2u);
}

TEST(TargetControllerTest, ClampRespectsSmallCapacity)
{
    TargetControllerParams params = make_params();
    params.capacity_slots = 4;
    TargetController controller(params);
    // initial=4 钳制到 [2,4]；大抖动期望超容量也被钳住。
    EXPECT_EQ(controller.current(), 4u);
    EXPECT_EQ(controller.update(0.0, 100.0, 1'000'000'000), 4u);
}

TEST(TargetControllerTest, BaseDelayLiftsTarget)
{
    TargetController controller(make_params());
    // base=25ms（2.5 包）+ J=0 → 期望 ceil(2.5) = 3。
    EXPECT_EQ(controller.update(25.0, 0.0, 1'000'000'000), 3u);
}

// estimator → controller 联动：合成 trace 驱动全链，验收 §13 四条轨迹。
struct TraceFeeder {
    JitterEstimator estimator { 48000, 480 };
    TargetController controller { make_params() };
    std::uint16_t seq = 0;
    std::uint32_t timestamp = 0;
    std::int64_t arrival_ns = 1'000'000'000;

    std::uint32_t packet(double arrival_jitter_ms = 0.0)
    {
        timestamp += 480;
        arrival_ns += kPacketNs + static_cast<std::int64_t>(arrival_jitter_ms * 1'000'000.0);
        estimator.observe(seq, timestamp, 0xABu, arrival_ns);
        ++seq;
        const auto estimates = estimator.estimates();
        return controller.update(estimates.base_delay_ms, estimates.jitter_ms, arrival_ns);
    }
};

TEST(TargetControllerTest, EndToEndCleanLanDropsAndJitterRaises)
{
    TraceFeeder feeder;
    // clean 200 包：target 从 4 降到 min=2。
    for (int i = 0; i < 200; ++i) {
        feeder.packet();
    }
    EXPECT_EQ(feeder.controller.current(), 2u);

    // ±20ms 抖动 200 包：J→~20，期望 ceil(2×20/10)=4，超死区涨到 ≥4。
    for (int i = 0; i < 200; ++i) {
        feeder.packet(i % 2 == 0 ? 20.0 : -20.0);
    }
    EXPECT_GE(feeder.controller.current(), 4u);
    const auto peak = feeder.controller.current();

    // 恢复干净 1s（100 包）：限速 1 格/s，最多降 1 格。
    for (int i = 0; i < 100; ++i) {
        feeder.packet();
    }
    EXPECT_GE(feeder.controller.current(), peak - 1);
    EXPECT_LE(feeder.controller.current(), peak);
}

TEST(TargetControllerTest, EndToEndSteadyJitterDoesNotOscillate)
{
    TraceFeeder feeder;
    for (int i = 0; i < 100; ++i) {
        feeder.packet();
    }
    // 稳定 ±8ms 抖动 30s：记录 target 极差，不许锯齿。
    std::uint32_t floor = feeder.controller.current();
    std::uint32_t ceiling = floor;
    for (int i = 0; i < 3000; ++i) {
        const auto target = feeder.packet(i % 2 == 0 ? 8.0 : -8.0);
        floor = std::min(floor, target);
        ceiling = std::max(ceiling, target);
    }
    EXPECT_LE(ceiling - floor, 2u);
}

} // namespace
