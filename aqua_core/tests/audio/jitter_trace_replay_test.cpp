// JBT trace 回放：把线上 `--jb-trace` 日志喂回**真实**三件套，做同 trace A/B。
//
// 为什么需要它：tail vs legacy 的 live 对照做不公平——两次跑的网络实现不同
// （stall 次数/幅度不同），无法归因。同一份到达时刻表分别驱动两种策略，
// 差值才干净。公式、限速、风暴、带几何、计罚口径全部来自生产代码（与
// jitter_control_replay_test.cpp 同一套接线：apply_adaptive_bands +
// select_penalty_events），不可能漂移。
//
// 用法：AQUA_JB_TRACE=/path/to/client.log 运行本文件测试；未设置/不可读则
// GTEST_SKIP（CI 常绿）。JBT 行新旧格式都认（旧格式无 tr_ms/trlvl_ms/tstep
// 三列，直接忽略；回放只需要 seq/ts/arr_ns）。
//
// 模型边界（与合成 harness 一致，刻意如此）：
//   - 到达时刻来自 trace（真实网络），消费节奏是合成的（512 帧/10.667ms，
//     16 档相位扫描——只看一个相位会严重低估欠载）；
//   - 样本内容是常量填充（不关心音频质量，只关心 target/欠载/集数）；
//   - SSRC 在 JBT 行里没有，整条 trace 按同一流处理（SSRC 切换本来就是会话
//     边界，真 trace 里若有也只在首尾）；
//   - 声道数压成单声道（控制决策与声道数无关，conceal/splice 的逐样本数学同构）。

#include "aqua/audio/buffer/jitter_buffer.h"

#include "aqua/audio/audio_format.h"
#include "aqua/audio/audio_frame.h"
#include "aqua/audio/buffer/buffer_config.h"
#include "aqua/audio/buffer/jitter_estimator.h"
#include "aqua/audio/buffer/target_controller.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

using aqua::audio::AudioEncoding;
using aqua::audio::AudioFormat;
using aqua::audio::AudioFrame;
using aqua::audio::JitterBuffer;
using aqua::audio::JitterBufferConfig;
using aqua::audio::JitterEstimator;
using aqua::audio::TargetController;
using aqua::audio::TargetControllerParams;
using aqua::audio::TargetMarginStrategy;

// ---- 与合成 harness 同源的几何 ----
constexpr std::uint32_t kPullFrames = 512; // WASAPI 周期（实测 512）
constexpr int kPhases = 16;
constexpr std::uint32_t kSsrc = 0xA0A0A0A0u;
constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr double kWarmupMs = 2000.0;

struct TracePacket {
    std::uint16_t seq = 0; // wire 序号（estimator 用原始值）
    std::uint64_t ext_seq = 0; // 展开后的单调序号（JB 用，跨 65535 回绕）
    std::uint32_t ts = 0; // RTP 时间戳
    std::int64_t t_ns = 0; // 到达时刻（首包归零后的相对值）
};

struct TraceGeometry {
    std::uint32_t capacity_slots = 30;
    std::uint32_t frame_count = 175;
    std::uint32_t sample_rate = 48000;
};

struct Trace {
    TraceGeometry geometry;
    std::vector<TracePacket> packets;
};

struct TraceMetrics {
    std::uint32_t target_min = 0;
    std::uint32_t target_max = 0;
    double target_mean = 0.0;
    double underrun_pct = 0.0;
    double penalty_max = 0.0;
    std::uint64_t fill_episodes = 0;
    std::uint64_t drop_episodes = 0;
    std::uint64_t reanchors = 0;
    bool operator==(const TraceMetrics&) const = default;
};

// JBT 行新旧格式都认：回放只需要 seq/ts/arr_ns，尾巴（jit/p99/…/tstep）是当时
// 的观测快照，直接忽略。seq 可能超过 16 位打印？不——JBT 打的是 uint16_t。
bool parse_jbt_line(const std::string& line, std::uint16_t& seq, std::uint32_t& ts,
    std::int64_t& arr_ns)
{
    static const std::regex kJbt(
        R"(JBT seq=(\d+) ts=(\d+) arr_ns=(-?\d+))", std::regex::optimize);
    std::smatch m;
    if (!std::regex_search(line, m, kJbt)) {
        return false;
    }
    const auto seq64 = static_cast<std::uint64_t>(std::stoull(m[1].str()));
    const auto ts64 = static_cast<std::uint64_t>(std::stoull(m[2].str()));
    if (seq64 > 0xFFFFu || ts64 > 0xFFFFFFFFu) {
        return false;
    }
    seq = static_cast<std::uint16_t>(seq64);
    ts = static_cast<std::uint32_t>(ts64);
    arr_ns = static_cast<std::int64_t>(std::stoll(m[3].str()));
    return true;
}

Trace parse_trace_text(const std::string& text)
{
    Trace trace;
    bool have_jb = false;
    bool have_client = false;
    // 日志是 aftermath 视角：先扫几何（若有），再扫包。
    static const std::regex kJbCreated(
        R"(JitterBuffer created: slots=(\d+) frame_count=(\d+))", std::regex::optimize);
    static const std::regex kClient(
        R"(\((\d+)ch/(\d+)Hz/enc=\d+, F=(\d+)\))", std::regex::optimize);
    std::istringstream in(text);
    std::string line;
    struct Raw {
        std::uint16_t seq;
        std::uint32_t ts;
        std::int64_t arr;
    };
    std::vector<Raw> raw;
    while (std::getline(in, line)) {
        if (!have_jb) {
            std::smatch m;
            if (std::regex_search(line, m, kJbCreated)) {
                trace.geometry.capacity_slots = static_cast<std::uint32_t>(std::stoul(m[1].str()));
                trace.geometry.frame_count = static_cast<std::uint32_t>(std::stoul(m[2].str()));
                have_jb = true;
            }
        }
        if (!have_client) {
            std::smatch m;
            if (std::regex_search(line, m, kClient)) {
                trace.geometry.sample_rate = static_cast<std::uint32_t>(std::stoul(m[2].str()));
                trace.geometry.frame_count = static_cast<std::uint32_t>(std::stoul(m[3].str()));
                have_client = true;
            }
        }
        std::uint16_t seq = 0;
        std::uint32_t ts = 0;
        std::int64_t arr = 0;
        if (parse_jbt_line(line, seq, ts, arr)) {
            raw.push_back(Raw { seq, ts, arr });
        }
    }
    if (raw.empty()) {
        return trace;
    }
    // 到达序：push strand 顺序打，文件序即到达序；仍按 arr 稳定排序防交错。
    std::stable_sort(raw.begin(), raw.end(),
        [](const Raw& a, const Raw& b) { return a.arr < b.arr; });
    const std::int64_t t0 = raw.front().arr;
    // seq 回绕展开（uint16 → 单调 uint64 给 JB；estimator 仍吃原始 uint16，
    // 它内部自己 extend）。
    std::uint64_t high = 0;
    std::uint16_t prev = raw.front().seq;
    bool first = true;
    trace.packets.reserve(raw.size());
    for (const auto& r : raw) {
        if (!first && r.seq < prev && static_cast<std::uint16_t>(prev - r.seq) > 0x8000u) {
            high += 0x10000u; // 前滚回绕
        }
        // 回退（重传/乱序）不倒 high：ext 用 max 语义会错，JB 侧本就按SlotHeader
        // 校验 sequence，这里只保证"回绕引起的前进"被展开。
        first = false;
        prev = r.seq;
        trace.packets.push_back(TracePacket { r.seq,
            high + static_cast<std::uint64_t>(r.seq), r.ts, r.arr - t0 });
    }
    return trace;
}

// 生产口径的 controller 参数（对应 client_runtime.cpp setup_playback）。
TargetControllerParams trace_params(TargetMarginStrategy strategy, const TraceGeometry& g)
{
    TargetControllerParams p;
    p.capacity_slots = std::max<std::uint32_t>(1,
        static_cast<std::uint32_t>(static_cast<double>(g.capacity_slots)
            * aqua::config::JB_ADAPTIVE_TARGET_CAPACITY_RATIO));
    p.packet_ms = static_cast<double>(g.frame_count) * 1000.0 / static_cast<double>(g.sample_rate);
    p.jitter_gain = aqua::config::JB_ADAPTIVE_DEFAULT_JITTER_GAIN;
    p.margin_strategy = strategy;
    p.min_target_slots = aqua::config::JB_ADAPTIVE_DEFAULT_MIN_TARGET_SLOTS;
    p.stall_peak_cap_slots = aqua::config::JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS;
    p.underrun_penalty_per_event = aqua::config::JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS;
    p.geometric_floor_slots = (kPullFrames + g.frame_count - 1) / g.frame_count;
    return p;
}

TraceMetrics run_trace(const Trace& trace, TargetMarginStrategy strategy, double phase_ms)
{
    const auto& g = trace.geometry;
    auto params = trace_params(strategy, g);
    params.initial_target_slots = std::min(
        TargetController::floor_target(params), params.capacity_slots);

    JitterBufferConfig cfg;
    cfg.capacity_slots = g.capacity_slots;
    cfg.format = AudioFormat { AudioEncoding::PCM_F32LE, 1, g.sample_rate };
    cfg.frame_count = g.frame_count;
    cfg.concealment.enabled = true; // 产品默认开（组件默认关，由 Runtime 打开）
    cfg.concealment.max_slots = aqua::config::JB_CONCEALMENT_DEFAULT_MAX_SLOTS;
    cfg.splice.enabled = true;
    aqua::audio::apply_adaptive_bands(cfg, g.capacity_slots, params.initial_target_slots);

    auto created = JitterBuffer::create(cfg);
    if (!created) {
        return TraceMetrics { };
    }
    JitterBuffer& jb = **created;
    JitterEstimator estimator(g.sample_rate, g.frame_count,
        aqua::config::JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS,
        aqua::config::JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC);
    TargetController controller(params);

    const double pull_period_ms
        = static_cast<double>(kPullFrames) * 1000.0 / static_cast<double>(g.sample_rate);
    const std::int64_t end_ns = trace.packets.empty() ? 0 : trace.packets.back().t_ns;
    const std::uint32_t frame_bytes = 4; // F32LE 单声道
    std::vector<std::byte> payload(static_cast<std::size_t>(g.frame_count) * frame_bytes);
    std::vector<std::byte> output(static_cast<std::size_t>(kPullFrames) * frame_bytes);

    TraceMetrics m;
    std::uint64_t pulled = 0;
    std::uint64_t silent = 0;
    std::uint64_t target_sum = 0;
    std::uint64_t target_n = 0;
    std::uint32_t target_min = g.capacity_slots;
    std::uint32_t target_max = 0;
    bool warmed = false;

    std::size_t ai = 0;
    // pull 事件表：phase 偏移 + 固定周期，到 trace 末尾。
    std::int64_t next_pull_ns = static_cast<std::int64_t>(phase_ms * kNsPerMs);
    while (ai < trace.packets.size() || next_pull_ns <= end_ns) {
        const bool do_pull = ai >= trace.packets.size()
            || (next_pull_ns <= end_ns && next_pull_ns < trace.packets[ai].t_ns);
        if (!do_pull) {
            const auto& e = trace.packets[ai++];
            estimator.observe(e.seq, e.ts, kSsrc, e.t_ns);
            const auto estimates = estimator.estimates();
            const auto penalty_events = aqua::audio::select_penalty_events(
                cfg.concealment.enabled, jb.underrun_events(), jb.concealed_saturated_slots());
            const auto target = controller.update(estimates.jitter_ms, e.t_ns,
                penalty_events, estimates.stall_peak_ms, estimates.tail_p99_ms,
                estimates.stall_events);
            jb.set_target_slots(target);
            if (e.t_ns >= static_cast<std::int64_t>(kWarmupMs * kNsPerMs)) {
                target_sum += target;
                ++target_n;
                target_min = std::min(target_min, target);
                target_max = std::max(target_max, target);
                m.penalty_max = std::max(m.penalty_max, controller.underrun_penalty());
            }
            const auto fill = static_cast<std::byte>((e.ext_seq + 1) & 0xFF);
            std::fill(payload.begin(), payload.end(), fill);
            AudioFrame frame { e.ext_seq, g.frame_count,
                std::span<const std::byte>(payload) };
            (void)jb.push(frame);
            continue;
        }
        if (!warmed && next_pull_ns >= static_cast<std::int64_t>(kWarmupMs * kNsPerMs)) {
            warmed = true;
            pulled = 0;
            silent = 0;
        }
        const auto r = jb.pull(output);
        pulled += r.frames_filled;
        silent += r.silence_frames;
        next_pull_ns += static_cast<std::int64_t>(pull_period_ms * kNsPerMs);
    }

    m.target_min = target_n > 0 ? target_min : 0;
    m.target_max = target_max;
    m.target_mean = target_n > 0
        ? static_cast<double>(target_sum) / static_cast<double>(target_n)
        : 0.0;
    m.underrun_pct
        = 100.0 * static_cast<double>(silent) / static_cast<double>(std::max<std::uint64_t>(1, pulled));
    m.fill_episodes = jb.fill_episodes();
    m.drop_episodes = jb.drop_episodes();
    m.reanchors = jb.reanchor_count();
    return m;
}

std::optional<Trace> load_trace_from_env()
{
    // MSVC 把 std::getenv 标为 C4996（默认构建告警即错误），POSIX 上直接读。
    std::string path;
#ifdef _WIN32
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, "AQUA_JB_TRACE") != 0 || buf == nullptr) {
        return std::nullopt;
    }
    path = buf;
    std::free(buf);
#else
    const char* env = std::getenv("AQUA_JB_TRACE");
    if (env == nullptr) {
        return std::nullopt;
    }
    path = env;
#endif
    if (path.empty()) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << file.rdbuf();
    Trace trace = parse_trace_text(text.str());
    if (trace.packets.empty()) {
        return std::nullopt;
    }
    return trace;
}

// ---- 内联 snippet：锁 parser 语义（无文件也跑）----

TEST(JitterTraceReplayTest, ParsesMixedFormatSnippet)
{
    const char* text = R"LOG([2026-09-17 13:39:20.000] [aqua] [debug] JitterBuffer created: slots=30 frame_count=175 frame_bytes=8 slot_bytes=1400 startup_slots=4 target_slots=4 warning=[1,6] normal=[2,5] step=[1,3]
[2026-09-17 13:39:20.001] [aqua] [info] client: session=0xC185A682 server=192.168.120.183:50000 (2ch/48000Hz/enc=3, F=175)
[2026-09-17 13:39:20.002] [aqua] [debug] JBT seq=100 ts=1000 arr_ns=1000000000 jit_ms=0.100 p99_ms=-1.00 stall_peak_ms=0.0 target=4 lead=1
[2026-09-17 13:39:20.003] [aqua] [debug] JBT seq=101 ts=1175 arr_ns=1003645833 jit_ms=0.200 p99_ms=3.00 stall_peak_ms=0.0 target=4 lead=2 tr_ms=0.10 trlvl_ms=0.00 tstep=0
[2026-09-17 13:39:20.004] [aqua] [debug] JBT seq=102 ts=1350 arr_ns=1007291666 jit_ms=0.300 p99_ms=3.00 stall_peak_ms=0.0 target=4 lead=3 tr_ms=0.20 trlvl_ms=0.10 tstep=1
)LOG";
    const Trace trace = parse_trace_text(text);
    EXPECT_EQ(trace.geometry.capacity_slots, 30u);
    EXPECT_EQ(trace.geometry.frame_count, 175u);
    EXPECT_EQ(trace.geometry.sample_rate, 48000u);
    ASSERT_EQ(trace.packets.size(), 3u);
    EXPECT_EQ(trace.packets[0].seq, 100u);
    EXPECT_EQ(trace.packets[0].t_ns, 0);
    EXPECT_EQ(trace.packets[1].t_ns, 3645833);
    EXPECT_EQ(trace.packets[2].ts, 1350u);
    EXPECT_EQ(trace.packets[2].ext_seq, 102u);
}

TEST(JitterTraceReplayTest, ParsesDefaultsWithoutHeaders)
{
    const char* text = "JBT seq=7 ts=70 arr_ns=5000\nJBT seq=8 ts=245 arr_ns=8000\n";
    const Trace trace = parse_trace_text(text);
    EXPECT_EQ(trace.geometry.capacity_slots, 30u);
    EXPECT_EQ(trace.geometry.frame_count, 175u);
    EXPECT_EQ(trace.geometry.sample_rate, 48000u);
    ASSERT_EQ(trace.packets.size(), 2u);
    EXPECT_EQ(trace.packets[0].t_ns, 0);
    EXPECT_EQ(trace.packets[1].t_ns, 3000);
}

TEST(JitterTraceReplayTest, UnwrapsSequenceRollover)
{
    const char* text = "JBT seq=65534 ts=1 arr_ns=1000\nJBT seq=65535 ts=2 arr_ns=2000\nJBT seq=0 ts=3 arr_ns=3000\nJBT seq=1 ts=4 arr_ns=4000\n";
    const Trace trace = parse_trace_text(text);
    ASSERT_EQ(trace.packets.size(), 4u);
    EXPECT_EQ(trace.packets[0].ext_seq, 65534u);
    EXPECT_EQ(trace.packets[1].ext_seq, 65535u);
    EXPECT_EQ(trace.packets[2].ext_seq, 65536u);
    EXPECT_EQ(trace.packets[3].ext_seq, 65537u);
    // estimator 吃原始 uint16（它内部自己 extend），JB 吃展开值。
    EXPECT_EQ(trace.packets[2].seq, 0u);
}

TEST(JitterTraceReplayTest, RejectsGarbage)
{
    EXPECT_TRUE(parse_trace_text("").packets.empty());
    EXPECT_TRUE(parse_trace_text("JBT seq=999999999 ts=1 arr_ns=1\n").packets.empty());
    EXPECT_TRUE(parse_trace_text("no jb lines here\n").packets.empty());
}

// ---- 真 trace：同表 A/B（需 AQUA_JB_TRACE）----

TEST(JitterTraceReplayTest, ReplayIsDeterministic)
{
    auto trace = load_trace_from_env();
    if (!trace.has_value()) {
        GTEST_SKIP() << "set AQUA_JB_TRACE=/path/to/client.log to enable trace replay";
    }
    std::printf("\n[JB trace] packets=%llu duration=%.1fs F=%u rate=%u capacity=%u\n",
        static_cast<unsigned long long>(trace->packets.size()),
        static_cast<double>(trace->packets.back().t_ns) / 1e9,
        trace->geometry.frame_count, trace->geometry.sample_rate,
        trace->geometry.capacity_slots);
    const auto a = run_trace(*trace, TargetMarginStrategy::TailQuantile, 0.0);
    const auto b = run_trace(*trace, TargetMarginStrategy::TailQuantile, 0.0);
    EXPECT_EQ(a, b);
}

TEST(JitterTraceReplayTest, SameTraceTailVsLegacy)
{
    auto trace = load_trace_from_env();
    if (!trace.has_value()) {
        GTEST_SKIP() << "set AQUA_JB_TRACE=/path/to/client.log to enable trace replay";
    }
    const auto floor = std::min(TargetController::floor_target(trace_params(TargetMarginStrategy::TailQuantile, trace->geometry)),
        trace_params(TargetMarginStrategy::TailQuantile, trace->geometry).capacity_slots);
    const auto max_target = trace_params(TargetMarginStrategy::TailQuantile, trace->geometry).capacity_slots;
    std::printf("\n[JB trace] strategy A/B on %llu packets (floor=%u max=%u, worst of %d phases)\n",
        static_cast<unsigned long long>(trace->packets.size()), floor, max_target, kPhases);
    std::printf("%-8s %7s %7s %8s %8s %9s %6s %6s %6s\n", "strategy",
        "tgt_min", "tgt_max", "tgt_mean", "und%", "pen_max", "FILL", "DROP", "REANC");
    for (const auto strategy : { TargetMarginStrategy::TailQuantile, TargetMarginStrategy::ScaledJitter }) {
        const char* name = strategy == TargetMarginStrategy::TailQuantile ? "tail" : "legacy";
        double worst_und = -1.0;
        TraceMetrics worst { };
        for (int i = 0; i < kPhases; ++i) {
            const double pull_period_ms = static_cast<double>(kPullFrames) * 1000.0
                / static_cast<double>(trace->geometry.sample_rate);
            const auto m = run_trace(*trace, strategy, i * pull_period_ms / kPhases);
            // 结构不变式：任何输入下 target 不得出 [floor, max]。
            EXPECT_GE(m.target_min, floor);
            EXPECT_LE(m.target_max, max_target);
            if (m.underrun_pct > worst_und) {
                worst_und = m.underrun_pct;
                worst = m;
            }
        }
        std::printf("%-8s %7u %7u %8.2f %8.3f %9.2f %6llu %6llu %6llu\n", name,
            worst.target_min, worst.target_max, worst.target_mean, worst.underrun_pct,
            worst.penalty_max, static_cast<unsigned long long>(worst.fill_episodes),
            static_cast<unsigned long long>(worst.drop_episodes),
            static_cast<unsigned long long>(worst.reanchors));
    }
    SUCCEED();
}

} // namespace
