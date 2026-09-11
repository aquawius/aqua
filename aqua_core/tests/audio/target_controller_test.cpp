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
    // 显式钉住 k=2 并关掉 dwell：本文件上半部分测的是**控制语义**（涨快/跌慢/
    // 不振荡/钳制），不应随默认 k 或 dwell 取值而失效。默认取值（含几何地板）
    // 由下面 Default* / PullGrant* 用例单独钉，dwell 由 RiseDwell* 单独测。
    params.jitter_gain = 2.0;
    params.rise_dwell_ms = 0.0;
    return params; // min=3 initial=4 k=2 fall=1/s deadband=0 dwell=0
}

TEST(TargetControllerTest, CleanNetworkFallsToMinImmediately)
{
    TargetController controller(make_params());
    EXPECT_EQ(controller.current(), 4u);
    // 零抖动：期望 = ceil(0) → min=3，首拍无时间基全额跟进。
    EXPECT_EQ(controller.update(0.0, 0.0, 1'000'000'000), 3u);
    EXPECT_EQ(controller.update(0.0, 0.0, 1'000'000'000 + kPacketNs), 3u);
}

TEST(TargetControllerTest, JitterSpikeRisesFast)
{
    TargetController controller(make_params());
    controller.update(0.0, 0.0, 1'000'000'000);
    ASSERT_EQ(controller.current(), 3u);
    // J=30ms：期望 = ceil(2×30/10) = 6，超死区立即跟进（单拍）。
    EXPECT_EQ(controller.update(0.0, 30.0, 1'000'000'000 + kPacketNs), 6u);
}

TEST(TargetControllerTest, RecoveryFallsAtLimitedRate)
{
    TargetController controller(make_params());
    std::int64_t now = 1'000'000'000;
    controller.update(0.0, 30.0, now); // 首拍全额 → max(3, 6)=6
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
    EXPECT_EQ(controller.current(), 3u);
}

// 回归锁定：死区默认 1 时，跌侧"不限死区 grind 到底"会把 target 压到 desired，
// 之后 desired 只回升 1 格就被死区吞掉 → target 永久停在 desired−1。实测这条
// 链路上 desired=3（0% 欠载）而 target 卡在 2（16.6% 欠载 + 25% 丢帧）。
TEST(TargetControllerTest, ConvergesToDesiredWithoutPermanentOffset)
{
    TargetController controller(make_params());
    std::int64_t now = 1'000'000'000;
    ASSERT_EQ(controller.update(0.0, 0.0, now), 3u); // 干净网：desired = min = 3
    now += kPacketNs;
    // J=20ms → desired = ceil(2×20/10) = 4：只高 1 格也必须立即跟进。
    EXPECT_EQ(controller.update(0.0, 20.0, now), 4u);
    // desired 回落：跌侧限速，不允许瞬时掉回。
    for (int i = 0; i < 50; ++i) {
        now += kPacketNs;
        EXPECT_EQ(controller.update(0.0, 0.0, now), 4u) << "packet " << i;
    }
}

TEST(TargetControllerTest, SteadyDesiredDoesNotMoveTarget)
{
    TargetController controller(make_params());
    std::int64_t now = 1'000'000'000;
    controller.update(0.0, 0.0, now);
    ASSERT_EQ(controller.current(), 3u);
    // J 在 0~5ms 间摆动：期望恒为 min=3（2×5/10=1 < min），target 纹丝不动。
    for (int i = 0; i < 100; ++i) {
        now += kPacketNs;
        const double jitter = (i % 2 == 0) ? 0.0 : 5.0;
        controller.update(0.0, jitter, now);
    }
    EXPECT_EQ(controller.current(), 3u);
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

// ---- 默认取值与几何地板（回归锁定：这两个数是双机实测 + 离线仿真的结论）----

// 默认 k=5：Aqua 的 server 以 capture 周期成串发包（实测 J≈4.6ms @ F=3.75ms），
// 均值型 J 只是峰值的约一半，k=2 给出的 3 slots 在双机上周期性排空
// （6.5% 欠载 + 12% 丢帧）。k=5 → ceil(5×4.6/3.75) = 7 slots 才是 0 欠载。
TEST(TargetControllerTest, DefaultGainSizingOnBurstyLink)
{
    TargetControllerParams params;
    params.capacity_slots = 30;
    params.packet_ms = 3.75; // 180 帧 @48kHz
    TargetController controller(params);
    EXPECT_EQ(controller.update(0.0, 4.6, 1'000'000'000), 7u);
}

// 几何地板：一次 playback callback 消耗 geometric_floor 个包时，target 必须 ≥
// floor+1，否则每个 callback 都把 JB 抽空——与抖动无关的结构性下限。
TEST(TargetControllerTest, GeometricFloorRaisesFloor)
{
    TargetControllerParams params = make_params();
    params.geometric_floor_slots = 3; // 512 帧 callback / 180 帧每包
    TargetController controller(params);
    EXPECT_EQ(controller.min_target(), 4u);
    // 零抖动 + 零底噪：期望被地板抬到 4，而不是 min_target_slots 的 3。
    EXPECT_EQ(controller.update(0.0, 0.0, 1'000'000'000), 4u);
}

// 起步初值也不能低于地板（否则启动瞬间就落在结构性排空区）。
TEST(TargetControllerTest, GeometricFloorClampsInitialTarget)
{
    TargetControllerParams params = make_params();
    params.geometric_floor_slots = 4;
    TargetController controller(params);
    EXPECT_EQ(controller.min_target(), 5u);
    EXPECT_EQ(controller.current(), 5u);
}

// ---- 欠载反馈闭环（细则 §3：underrun history 是 controller 的输入）----

TEST(TargetControllerTest, UnderrunRaisesTargetFloorAboveJitterOnlyValue)
{
    TargetController controller(make_params());
    std::int64_t now = 1'000'000'000;
    ASSERT_EQ(controller.update(0.0, 0.0, now), 3u); // 干净网：期望 = min = 3
    now += kPacketNs;
    // 抖动不足以抬升（J=5ms → 2×5/10=1 < min），但发生 2 次欠载 →
    // 下限被顶到 3+2 = 5。预测项管不到的事，反馈项必须管到。
    EXPECT_EQ(controller.update(0.0, 5.0, now, 2u), 5u);
}

TEST(TargetControllerTest, UnderrunPenaltyDecaysBackWhenUnderrunsStop)
{
    TargetController controller(make_params());
    std::int64_t now = 1'000'000'000;
    controller.update(0.0, 0.0, now, 3u);
    ASSERT_EQ(controller.current(), 6u);
    // 不再欠载：衰减 0.5 槽/秒。0.5s（50 包 @10ms）后 penalty=2.5 → 下限 5。
    for (int i = 0; i < 50; ++i) {
        now += kPacketNs;
        controller.update(0.0, 0.0, now, 3u);
    }
    EXPECT_EQ(controller.min_target() + static_cast<std::uint32_t>(controller.underrun_penalty()), 5u);
    // 跌侧限速仍在（1 格/秒），所以 0.5s 最多降 1 格。
    EXPECT_GE(controller.current(), 5u);
    // 长期无欠载 → 回到纯预测项给出的 min。
    for (int i = 0; i < 800; ++i) {
        now += kPacketNs;
        controller.update(0.0, 0.0, now, 3u);
    }
    EXPECT_EQ(controller.current(), 3u);
    EXPECT_EQ(controller.underrun_penalty(), 0.0);
}

TEST(TargetControllerTest, UnderrunPenaltyIsCapped)
{
    TargetControllerParams params = make_params();
    params.underrun_penalty_max_slots = 4;
    TargetController controller(params);
    // 一次灌 100 次欠载：抬升被 max 封住，不会把整个 buffer 吃满。
    EXPECT_EQ(controller.update(0.0, 0.0, 1'000'000'000, 100u), 7u);
    EXPECT_LE(controller.underrun_penalty(), 4.0);
}

TEST(TargetControllerTest, UnderrunFeedbackDisabledByDefaultInComponent)
{
    TargetController controller(make_params());
    // 不传欠载计数（组件单独使用）：行为与 Phase 1 完全一致。
    EXPECT_EQ(controller.update(0.0, 0.0, 1'000'000'000), 3u);
    EXPECT_EQ(controller.underrun_penalty(), 0.0);
}

// floor_target() 必须与构造后的 min_target() 一致：ClientRuntime 用它决定起步
// 水位（那时 controller 还没建），两处口径一旦漂移就会出现"起步值低于几何
// 地板"的窗口。
TEST(TargetControllerTest, FloorTargetMatchesConstructedMinTarget)
{
    auto check = [](const TargetControllerParams& p) {
        TargetController controller(p);
        EXPECT_EQ(TargetController::floor_target(p), controller.min_target());
        EXPECT_GE(controller.current(), controller.min_target());
    };
    check(make_params());
    TargetControllerParams granted = make_params();
    granted.geometric_floor_slots = 3;
    check(granted);
    TargetControllerParams tiny = make_params();
    tiny.capacity_slots = 4;
    tiny.geometric_floor_slots = 8; // 地板超过容量：必须被钳到容量，不能溢出
    check(tiny);
}

// ---- 涨后 dwell 峰值保持（防 Wi-Fi 省电发包导致的 target 微跳）----

TEST(TargetControllerTest, RiseDwellPinsTargetWhileJitterOscillates)
{
    TargetControllerParams params = make_params();
    params.rise_dwell_ms = 3000.0;
    TargetController controller(params);
    std::int64_t now = 1'000'000'000;
    controller.update(0.0, 0.0, now);
    ASSERT_EQ(controller.current(), 3u);
    // 涨到 6（J=15 → ceil(2×15/10)=3... 用 30 → 6）。
    now += kPacketNs;
    ASSERT_EQ(controller.update(0.0, 30.0, now), 6u);
    // 随后 J 在"该降"与"该持平"间快速摆动（desired 3 ↔ 6），3s 窗口内：
    // target 必须钉在 6，一格不许跌。
    for (int i = 0; i < 20; ++i) { // 20 包 @10ms = 200ms
        now += kPacketNs;
        const auto target = controller.update(0.0, (i % 2 == 0) ? 0.0 : 30.0, now);
        EXPECT_EQ(target, 6u) << "dwell 窗口内不得下跌 packet " << i;
    }
    // 窗口（3s）过后，持续低 J → 允许回落。从 6 跌回 3 还需 3 槽 = 3s，
    // 加上 dwell 的 3s 共约 6s，跑 8s（800 包）确保到底。
    for (int i = 0; i < 800; ++i) {
        now += kPacketNs;
        controller.update(0.0, 0.0, now);
    }
    EXPECT_EQ(controller.current(), 3u);
}

TEST(TargetControllerTest, RiseDuringDwellRefreshesWindow)
{
    TargetControllerParams params = make_params();
    params.rise_dwell_ms = 3000.0;
    TargetController controller(params);
    std::int64_t now = 1'000'000'000;
    controller.update(0.0, 0.0, now);
    now += kPacketNs;
    ASSERT_EQ(controller.update(0.0, 30.0, now), 6u); // 涨到 6，记下 t1
    // 窗口内又一次恶化（desired 更高）→ 仍然即时上涨，且窗口从新涨点重算。
    for (int i = 0; i < 200; ++i) { // 2s 后
        now += kPacketNs;
        controller.update(0.0, 30.0, now);
    }
    now += kPacketNs;
    ASSERT_EQ(controller.update(0.0, 80.0, now), 16u); // ceil(2×80/10)=16，立即涨
    // 新窗口内（3s）J 掉到 0 → 仍锁跌。
    for (int i = 0; i < 200; ++i) { // 2s
        now += kPacketNs;
        EXPECT_EQ(controller.update(0.0, 0.0, now), 16u);
    }
}

TEST(TargetControllerTest, ZeroDwellDisablesPeakHold)
{
    TargetControllerParams params = make_params();
    params.rise_dwell_ms = 0.0; // 关 dwell：退回纯限速行为
    TargetController controller(params);
    std::int64_t now = 1'000'000'000;
    controller.update(0.0, 30.0, now);
    ASSERT_EQ(controller.current(), 6u);
    // 持续低 J：1 槽/秒限速，1s（100 包）后降 1 格。
    for (int i = 0; i < 100; ++i) {
        now += kPacketNs;
        controller.update(0.0, 0.0, now);
    }
    EXPECT_EQ(controller.current(), 5u);
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
    // clean 200 包：target 从 4 降到 min=3。
    for (int i = 0; i < 200; ++i) {
        feeder.packet();
    }
    EXPECT_EQ(feeder.controller.current(), 3u);

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
