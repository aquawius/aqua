#include "core/jitter_buffer/jitter_buffer.h"
#include "core/public/audio_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace {

// 测试用 AudioFormat: 48kHz, 2ch, F32LE
aqua::AudioFormat make_test_format() {
    aqua::AudioFormat fmt;
    fmt.encoding = aqua::AudioEncoding::PcmF32LE;
    fmt.channels = 2;
    fmt.sample_rate = 48000;
    return fmt;
}

// 48kHz, 10ms → 480 frames per packet
constexpr std::uint32_t FRAMES_PER_PACKET = 480;

// 每包 PCM 字节数: 480 * 2ch * 4 bytes = 3840 bytes
constexpr std::size_t PAYLOAD_SIZE = 480 * 2 * 4;

// target=3, capacity=8
constexpr std::size_t TARGET = 3;
constexpr std::size_t CAPACITY = 8;

// 生成指定 sequence 的 payload：每个字节填 sequence 的低 8 位
std::vector<std::byte> make_payload(std::uint32_t sequence, std::size_t size = PAYLOAD_SIZE) {
    std::vector<std::byte> payload(size);
    std::byte fill = static_cast<std::byte>(sequence & 0xFF);
    std::fill(payload.begin(), payload.end(), fill);
    return payload;
}

// 检查输出是否是某个 sequence 的 payload
bool is_payload_of(std::span<const std::byte> data, std::uint32_t sequence) {
    std::byte expected = static_cast<std::byte>(sequence & 0xFF);
    return std::all_of(data.begin(), data.end(), [expected](std::byte b) {
        return b == expected;
    });
}

// 检查输出是否全零（静音）
bool is_silence(std::span<const std::byte> data) {
    return std::all_of(data.begin(), data.end(), [](std::byte b) {
        return b == std::byte{0};
    });
}

// 检查输出是否为指定 sequence payload 的 PLC 衰减版本（F32 格式，gain 为 2 的幂时精确）。
bool is_plc_of(std::span<const std::byte> data, std::uint32_t sequence, float gain) {
    if (data.size() != PAYLOAD_SIZE) return false;
    // pattern 字节组成的 float 源值
    std::byte pat = static_cast<std::byte>(sequence & 0xFF);
    std::array<std::byte, 4> src = { pat, pat, pat, pat };
    float base;
    std::memcpy(&base, src.data(), sizeof(base));
    const float expected = base * gain;
    for (std::size_t i = 0; i < data.size(); i += 4) {
        float v;
        std::memcpy(&v, data.data() + i, sizeof(v));
        if (v != expected) return false;
    }
    return true;
}

// push 并检测是否触发了 rebase。
// 用 rebases() 计数器检测（最直接可靠）：deadline/next_sequence 在 diff==0 的
// rebase（drift 窗口落在 expected 包上）下都不变化，无法作为信号。
bool push_and_check_rebase(aqua::jitter::JitterBuffer& jb, std::uint32_t seq) {
    auto payload = make_payload(seq);
    const auto rebases_before = jb.rebases();
    jb.push(seq, payload);
    return jb.rebases() > rebases_before;
}

} // namespace

// ---- 基本功能 ----

TEST(JitterBufferTest, NormalInOrderPlayback) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 3 包
    for (std::uint32_t seq = 100; seq < 103; ++seq) {
        auto payload = make_payload(seq);
        jb.push(seq, payload);
    }

    // deadline 应该可用（第一个包后即建立 timeline）
    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // pop 3 包，应该按顺序输出
    for (std::uint32_t seq = 100; seq < 103; ++seq) {
        EXPECT_TRUE(jb.pop_next(out));
        EXPECT_TRUE(is_payload_of(out, seq));
    }

    EXPECT_EQ(jb.packets_received(), 3);
    EXPECT_EQ(jb.packets_lost(), 0);
    EXPECT_EQ(jb.duplicates(), 0);
    EXPECT_EQ(jb.late_packets(), 0);
}

TEST(JitterBufferTest, FirstPacketSetsDeadline) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);

    // 尚未收到任何包：deadline 不可用
    EXPECT_FALSE(jb.next_playout_deadline().has_value());

    // push 1 包
    jb.push(100, make_payload(100));

    // 第一个包后：deadline 应该可用（= first_packet_time + target_latency * packet_duration）
    auto deadline = jb.next_playout_deadline();
    EXPECT_TRUE(deadline.has_value());

    // deadline 应该在未来（target_latency = 30ms）
    auto now = std::chrono::steady_clock::now();
    EXPECT_GT(*deadline, now);
}

TEST(JitterBufferTest, ReorderProducesCorrectOrder) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 乱序 push: 100, 102, 101, 103
    jb.push(100, make_payload(100));
    jb.push(102, make_payload(102));
    jb.push(101, make_payload(101));
    jb.push(103, make_payload(103));

    // deadline 可用
    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // pop 应该按 100, 101, 102, 103 顺序输出
    for (std::uint32_t seq = 100; seq < 104; ++seq) {
        EXPECT_TRUE(jb.pop_next(out));
        EXPECT_TRUE(is_payload_of(out, seq)) << "Expected seq " << seq;
    }

    EXPECT_EQ(jb.packets_received(), 4);
    EXPECT_EQ(jb.packets_lost(), 0);
    EXPECT_EQ(jb.duplicates(), 0);
    EXPECT_EQ(jb.late_packets(), 0);
}

// ---- 丢包测试 ----

TEST(JitterBufferTest, SinglePacketLossConcealedWithDecayedRepeat) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 100, 101, 103（缺 102）
    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(103, make_payload(103));

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // pop: 100, 101 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 100));
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 101));

    // 102 丢包：PLC 输出上一包（101）PCM 的 0.5 倍衰减，而非静音
    EXPECT_FALSE(jb.pop_next(out));
    EXPECT_TRUE(is_plc_of(out, 101, 0.5f));

    // 103 正常（真实包恢复 PLC 增益）
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 103));

    EXPECT_EQ(jb.packets_lost(), 1);
}

TEST(JitterBufferTest, ConsecutivePacketLossDecaysToSilence) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 100, 101, 105（缺 102, 103, 104）
    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(105, make_payload(105));

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // 100, 101 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 100));
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 101));

    // 102, 103, 104 连续丢包：增益序列 0.5, 0.25, 0.125（基于 101 的 PCM）
    EXPECT_FALSE(jb.pop_next(out));
    EXPECT_TRUE(is_plc_of(out, 101, 0.5f));
    EXPECT_FALSE(jb.pop_next(out));
    EXPECT_TRUE(is_plc_of(out, 101, 0.25f));
    EXPECT_FALSE(jb.pop_next(out));
    EXPECT_TRUE(is_plc_of(out, 101, 0.125f));

    // 105 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 105));

    EXPECT_EQ(jb.packets_lost(), 3);
}

// ---- 重复包测试 ----

TEST(JitterBufferTest, DuplicatePacketDiscarded) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 100, 101, 101（重复）, 102
    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(101, make_payload(101));  // 重复
    jb.push(102, make_payload(102));

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // pop 3 包，应正常输出 100, 101, 102
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 100));
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 101));
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 102));

    EXPECT_EQ(jb.duplicates(), 1);
    EXPECT_EQ(jb.packets_lost(), 0);
}

// ---- Late packet 测试 ----

TEST(JitterBufferTest, LatePacketBeforeDeadlinePlaysCorrectly) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 100, 101, 103（缺 102）
    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(103, make_payload(103));

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // pop 100 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 100));

    // 在 102 的 deadline 之前 push 102
    jb.push(102, make_payload(102));

    // pop 101, 102 应该正常（不是 late）
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 101));
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 102));

    // 103 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 103));

    EXPECT_EQ(jb.late_packets(), 0);
    EXPECT_EQ(jb.packets_lost(), 0);
}

TEST(JitterBufferTest, LatePacketAfterDeadlineDiscarded) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 100, 101, 103
    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(103, make_payload(103));

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // pop 100, 101 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 100));
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 101));

    // pop 102 → 丢包隐藏（上一包 101 的 0.5 倍衰减）
    EXPECT_FALSE(jb.pop_next(out));
    EXPECT_TRUE(is_plc_of(out, 101, 0.5f));

    // 103 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 103));

    // 现在 push 102 → 应该被判定为 late（因为 next_pop_seq_ 已经是 104）
    jb.push(102, make_payload(102));

    EXPECT_EQ(jb.late_packets(), 1);
    EXPECT_EQ(jb.packets_lost(), 1);
}

// ---- Sequence 回绕测试 ----

TEST(JitterBufferTest, SequenceWraparound) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 从 0xFFFFFFFE 开始，跨越回绕点
    std::uint32_t start = 0xFFFFFFFE;
    for (std::uint32_t i = 0; i < 5; ++i) {
        std::uint32_t seq = start + i;  // 自动回绕
        jb.push(seq, make_payload(seq));
    }

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // pop 应按顺序输出 0xFFFFFFFE, 0xFFFFFFFF, 0x00000000, 0x00000001, 0x00000002
    for (std::uint32_t i = 0; i < 5; ++i) {
        std::uint32_t expected_seq = start + i;
        EXPECT_TRUE(jb.pop_next(out));
        EXPECT_TRUE(is_payload_of(out, expected_seq))
            << "Expected seq 0x" << std::hex << expected_seq << " at index " << i;
    }

    EXPECT_EQ(jb.packets_lost(), 0);
}

// ---- 巨大跳跃测试 ----

TEST(JitterBufferTest, HugeSequenceJumpTriggersReset) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 正常 push 3 包
    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(102, make_payload(102));

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // pop 100
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 100));

    // 巨大跳跃：push 500（远超 capacity=8）
    jb.push(500, make_payload(500));
    jb.push(501, make_payload(501));
    jb.push(502, make_payload(502));

    // 应该触发 reset，以 500 为新基准
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 500));
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 501));
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 502));

    // 统计应保留（reset 不清除统计）
    EXPECT_GE(jb.packets_received(), 6);
}

// ---- 软 rebase：跳跃后保留已缓冲的 future 包 ----

TEST(JitterBufferTest, SoftRebasePreservesFuturePackets) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 正常 push 100-103（capacity=8，都在窗口内）
    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(102, make_payload(102));
    jb.push(103, make_payload(103));

    // pop 100-101，next_pop_seq_ = 102
    (void)jb.pop_next(out);  // 100
    (void)jb.pop_next(out);  // 101

    // 巨大跳跃：push 110（diff = 110 - 102 = 8 >= capacity=8）
    // 软 rebase 应将 next_pop_seq_ 跳到 110，但不清除 slot。
    // 102 和 103 仍在 slot 中，但它们的 seq < 110（新 next_pop_seq_），
    // 所以 pop 时会被跳过（丢包隐藏填充）。
    jb.push(110, make_payload(110));

    // pop 应返回 110（rebase 基准包）
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 110));

    // 111-112 未推送：丢包隐藏（110 的衰减重复，0.5 / 0.25）
    EXPECT_FALSE(jb.pop_next(out));  // 111: PLC
    EXPECT_TRUE(is_plc_of(out, 110, 0.5f));
    EXPECT_FALSE(jb.pop_next(out));  // 112: PLC
    EXPECT_TRUE(is_plc_of(out, 110, 0.25f));

    // 统计应保留
    EXPECT_GE(jb.packets_received(), 5);
    EXPECT_GT(jb.packets_lost(), 0);  // 102-109, 111-112 等丢失
}

// ---- payload 大小校验 ----

TEST(JitterBufferTest, PayloadSizeMismatchIgnored) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);

    // push 一个大小不匹配的 payload
    std::vector<std::byte> wrong_size(PAYLOAD_SIZE / 2);
    jb.push(100, wrong_size);

    // 不应该被接收，但应计入 malformed 统计
    EXPECT_EQ(jb.packets_received(), 0);
    EXPECT_EQ(jb.malformed_packets(), 1);
    EXPECT_FALSE(jb.next_playout_deadline().has_value());
}

// ---- 连续正常运行 ----

TEST(JitterBufferTest, ContinuousNormalOperation) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 模拟 100 个包的连续收发
    // 初始缓冲 3 包，然后每收到 1 包就 pop 1 包
    for (std::uint32_t seq = 0; seq < 3; ++seq) {
        jb.push(seq, make_payload(seq));
    }

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    for (std::uint32_t seq = 0; seq < 100; ++seq) {
        // pop 当前包
        EXPECT_TRUE(jb.pop_next(out)) << "Failed at seq " << seq;
        EXPECT_TRUE(is_payload_of(out, seq)) << "Wrong payload at seq " << seq;

        // push 下一个包（如果还有）
        std::uint32_t next_push = seq + 3;
        if (next_push < 100) {
            jb.push(next_push, make_payload(next_push));
        }
    }

    EXPECT_EQ(jb.packets_lost(), 0);
    EXPECT_EQ(jb.duplicates(), 0);
    EXPECT_EQ(jb.late_packets(), 0);
    EXPECT_EQ(jb.packets_received(), 100);
}

// ---- buffer_fill_packets 统计 ----

TEST(JitterBufferTest, BufferFillPackets) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);

    // 初始为空
    EXPECT_EQ(jb.buffer_fill_packets(), 0);

    // push 3 包
    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(102, make_payload(102));

    // 应有 3 包
    EXPECT_EQ(jb.buffer_fill_packets(), 3);
}

// ---- reset 测试 ----

TEST(JitterBufferTest, ResetClearsPlayoutStateButNotStatistics) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 3 包 + pop 1 包（制造一些统计）
    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(102, make_payload(102));
    EXPECT_EQ(jb.packets_received(), 3);

    EXPECT_TRUE(jb.pop_next(out));  // pop 100

    // reset
    jb.reset();

    // 播放状态应清除
    EXPECT_FALSE(jb.next_playout_deadline().has_value());
    EXPECT_EQ(jb.buffer_fill_packets(), 0);

    // 统计应保留
    EXPECT_EQ(jb.packets_received(), 3);

    // reset 后可以重新开始
    jb.push(200, make_payload(200));
    EXPECT_EQ(jb.packets_received(), 4);
    EXPECT_EQ(jb.next_sequence(), 200);
    EXPECT_TRUE(jb.next_playout_deadline().has_value());
}

// ---- next_sequence 返回值 ----

TEST(JitterBufferTest, NextSequenceAfterPush) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);

    // push 1 包
    jb.push(500, make_payload(500));
    EXPECT_EQ(jb.next_sequence(), 500);

    std::vector<std::byte> out(PAYLOAD_SIZE);
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_EQ(jb.next_sequence(), 501);
}

// ---- pop 输出大小不足时返回 false ----

TEST(JitterBufferTest, PopOutputBufferTooSmall) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);

    // push 1 包
    jb.push(0, make_payload(0));

    // output 太小
    std::vector<std::byte> small_out(PAYLOAD_SIZE / 2);
    EXPECT_FALSE(jb.pop_next(small_out));
}

// ---- pop 在首个包到达前返回 false（防御时间线未初始化）----

TEST(JitterBufferTest, PopNextBeforeFirstPacketReturnsFalse) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 未 push 任何包：不应推进时间线，也不应计数 lost
    EXPECT_FALSE(jb.pop_next(out));
    EXPECT_EQ(jb.packets_lost(), 0);
    EXPECT_EQ(jb.next_sequence(), 0);
}

// ---- capacity 必须是 2 的幂 ----

TEST(JitterBufferTest, NonPowerOfTwoCapacityThrows) {
    EXPECT_THROW(
        aqua::jitter::JitterBuffer(make_test_format(), FRAMES_PER_PACKET, TARGET, 6),
        std::invalid_argument);

    EXPECT_THROW(
        aqua::jitter::JitterBuffer(make_test_format(), FRAMES_PER_PACKET, TARGET, 0),
        std::invalid_argument);

    // 2 的幂不抛异常
    EXPECT_NO_THROW(
        aqua::jitter::JitterBuffer(make_test_format(), FRAMES_PER_PACKET, TARGET, 8));

    EXPECT_NO_THROW(
        aqua::jitter::JitterBuffer(make_test_format(), FRAMES_PER_PACKET, TARGET, 16));
}

// ---- 构造参数校验 ----

TEST(JitterBufferTest, RejectsInvalidFormat) {
    aqua::AudioFormat invalid; // encoding=Invalid, channels=0, sample_rate=0
    EXPECT_THROW(
        aqua::jitter::JitterBuffer(invalid, FRAMES_PER_PACKET, TARGET, CAPACITY),
        std::invalid_argument);
}

TEST(JitterBufferTest, RejectsZeroFramesPerPacket) {
    EXPECT_THROW(
        aqua::jitter::JitterBuffer(make_test_format(), 0, TARGET, CAPACITY),
        std::invalid_argument);
}

TEST(JitterBufferTest, RejectsCapacityBelowTargetTwice) {
    // capacity 必须 >= target * 2（§22.9 契约）。4 < 4*2，8 < 8*2 → 抛。
    EXPECT_THROW(
        aqua::jitter::JitterBuffer(make_test_format(), FRAMES_PER_PACKET, 4, 4),
        std::invalid_argument);
    EXPECT_THROW(
        aqua::jitter::JitterBuffer(make_test_format(), FRAMES_PER_PACKET, 8, 8),
        std::invalid_argument);
    // target=4, capacity=8 恰好满足，不抛
    EXPECT_NO_THROW(
        aqua::jitter::JitterBuffer(make_test_format(), FRAMES_PER_PACKET, 4, 8));
}

// ---- 连续 late 触发 reset（音频源暂停后恢复）----
// 模拟切歌场景：server 暂停发包，JB 调度器持续空转 pop 推进 next_pop_seq_，
// 恢复后新包全部 diff<0（late），无法触发 diff>=capacity 的 reset。
// 连续 late 达到 capacity 时应强制 reset 重建时间线。

TEST(JitterBufferTest, ConsecutiveLateTriggersResetOnSourceResume) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 初始正常推送 seq 100-102
    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(102, make_payload(102));

    // pop 全部，next_pop_seq_ 推进到 103
    (void)jb.pop_next(out);  // 100
    (void)jb.pop_next(out);  // 101
    (void)jb.pop_next(out);  // 102

    // 模拟音频源暂停：JB 调度器继续空转 pop（无人发数据），
    // next_pop_seq_ 被推进 20 包（远超 capacity=8）
    for (int i = 0; i < 20; ++i) {
        (void)jb.pop_next(out);  // 全部静音填充，next_pop_seq_ 推进到 123
    }
    EXPECT_EQ(jb.next_sequence(), 123);

    // 音频源恢复，server 从 seq 103 继续发送
    // 此时 103..122 全部是 late（diff < 0），连续 late 应触发 reset
    for (std::uint32_t seq = 103; seq < 103 + CAPACITY; ++seq) {
        jb.push(seq, make_payload(seq));
    }
    // 第 CAPACITY 个 late 包应触发 reset，以最后到达的包重建时间线
    // reset 后 next_sequence 应等于触发 reset 的那个包的 seq
    std::uint32_t expected_reset_seq = 103 + CAPACITY - 1;  // 110
    EXPECT_EQ(jb.next_sequence(), expected_reset_seq);

    // reset 后应能正常播放触发 reset 的包
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, expected_reset_seq));

    // 后续包应正常工作（push 111 后 pop）
    jb.push(111, make_payload(111));
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 111));

    // 统计应保留（late 计数应包含空转期间的丢包 + 恢复期的 late）
    EXPECT_GT(jb.late_packets(), 0);
    EXPECT_GT(jb.packets_lost(), 0);
}

// ---- 时钟漂移检测 ----

// 模拟 server 时钟慢于 client：client 消费略快于 server 生产。
// 每轮排空缓冲后额外 pop 1 次（静音），使 next_pop_seq_ 超前 1，
// 随后 push 被跳过的 seq → late。每 11 包中 1 包 late（~9%），超过 1.5% 阈值。
TEST(JitterBufferTest, ClockDriftSlowServerTriggersRebase) {
    constexpr std::size_t D_TARGET = 4;
    constexpr std::size_t D_CAPACITY = 32;
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, D_TARGET, D_CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    std::uint32_t seq = 0;
    // 初始缓冲 10 包
    for (int i = 0; i < 10; ++i) {
        (void)push_and_check_rebase(jb, seq);
        ++seq;
    }

    bool rebase_triggered = false;
    for (int round = 0; round < 100 && !rebase_triggered; ++round) {
        // 排空 10 包
        for (int i = 0; i < 10; ++i) (void)jb.pop_next(out);
        // 额外 pop（静音），next_pop_seq_ 超前 1
        (void)jb.pop_next(out);
        // push 被跳过的 seq → late
        if (push_and_check_rebase(jb, seq)) rebase_triggered = true;
        ++seq;
        // 补充 10 包（expected/future）
        for (int i = 0; i < 10; ++i) {
            if (push_and_check_rebase(jb, seq)) rebase_triggered = true;
            ++seq;
        }
    }

    EXPECT_TRUE(rebase_triggered) << "漂移 rebase 应在 ~9% late rate 下触发";
    EXPECT_GT(jb.late_packets(), 15);
}

// 低 late rate（~0.5%），不触发 rebase
TEST(JitterBufferTest, LowLateRatioNoRebase) {
    constexpr std::size_t D_TARGET = 4;
    constexpr std::size_t D_CAPACITY = 128;
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, D_TARGET, D_CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    std::uint32_t seq = 0;
    for (int i = 0; i < 100; ++i) {
        (void)push_and_check_rebase(jb, seq);
        ++seq;
    }

    bool rebase_triggered = false;
    for (int round = 0; round < 25; ++round) {
        for (int i = 0; i < 100; ++i) (void)jb.pop_next(out);
        (void)jb.pop_next(out);  // 额外 pop（静音）
        if (push_and_check_rebase(jb, seq)) rebase_triggered = true;
        ++seq;
        for (int i = 0; i < 100; ++i) {
            if (push_and_check_rebase(jb, seq)) rebase_triggered = true;
            ++seq;
        }
    }

    EXPECT_FALSE(rebase_triggered) << "~0.5% late rate 不应触发 rebase";
}

// Windows 定时器批量交付模式：push 5 + pop 5，无 late，不触发 rebase
TEST(JitterBufferTest, BurstyDeliveryNoFalseRebase) {
    constexpr std::size_t D_TARGET = 4;
    constexpr std::size_t D_CAPACITY = 32;
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, D_TARGET, D_CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    std::uint32_t seq = 0;
    bool rebase_triggered = false;
    for (int round = 0; round < 200; ++round) {
        for (int i = 0; i < 5; ++i) {
            if (push_and_check_rebase(jb, seq)) rebase_triggered = true;
            ++seq;
        }
        for (int i = 0; i < 5; ++i) (void)jb.pop_next(out);
    }

    EXPECT_FALSE(rebase_triggered);
    EXPECT_EQ(jb.late_packets(), 0);
}

// rebase 后窗口重置：触发 rebase 后，1000 包无 late 不应再次触发
TEST(JitterBufferTest, DriftRebaseResetsWindow) {
    constexpr std::size_t D_TARGET = 4;
    constexpr std::size_t D_CAPACITY = 32;
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, D_TARGET, D_CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    std::uint32_t seq = 0;
    for (int i = 0; i < 10; ++i) {
        (void)push_and_check_rebase(jb, seq);
        ++seq;
    }

    // Phase 1: 触发 drift rebase（~9% late rate）
    bool rebase_triggered = false;
    for (int round = 0; round < 100 && !rebase_triggered; ++round) {
        for (int i = 0; i < 10; ++i) (void)jb.pop_next(out);
        (void)jb.pop_next(out);
        if (push_and_check_rebase(jb, seq)) rebase_triggered = true;
        ++seq;
        for (int i = 0; i < 10; ++i) {
            if (push_and_check_rebase(jb, seq)) rebase_triggered = true;
            ++seq;
        }
    }
    ASSERT_TRUE(rebase_triggered);

    // Phase 2: 1000 包 clean push-pop，验证窗口已重置、无 rebase
    auto late_after_rebase = jb.late_packets();
    for (int i = 0; i < 1000; ++i) {
        if (push_and_check_rebase(jb, seq)) {
            FAIL() << "Clean phase 中不应触发 rebase (seq=" << seq << ")";
        }
        (void)jb.pop_next(out);
        ++seq;
    }
    EXPECT_EQ(jb.late_packets(), late_after_rebase);
}

// ---- rebase 保持节奏：deadline 按缺口大小分档 ----
// 48kHz / 480 帧包 = 10ms/包。分档策略：
//   小前跳 (0 < diff <= target)：deadline 沿原 cadence 推进 diff 拍（供给不停顿）
//   大断裂 (diff > target)：重新缓冲 now + target×duration
//   回跳   (diff < 0)：下一拍播放（pop 空转场景，供给不停顿）

TEST(JitterBufferTest, RebaseSmallGapAdvancesDeadlineByCadence) {
    // 缩小 drift 窗口（4 包、阈值 1）快速构造 drift rebase，并使 rebase 包 diff ∈ (0, target]
    constexpr std::size_t R_TARGET = 4;
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, R_TARGET, 32,
                                  /*drift_window_size=*/4, /*drift_late_threshold=*/1);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 首包 seq=0 建立时间线；pop 2 次使 next_pop_seq_=2（制造 late 空间）
    jb.push(0, make_payload(0));
    (void)jb.pop_next(out);
    (void)jb.pop_next(out);

    // seq=1 落后于 next_pop_seq_=2 → late（窗口 late=1）
    jb.push(1, make_payload(1));
    // 填满窗口：seq=3,4,5 为 expected/future（total=4）
    jb.push(3, make_payload(3));
    jb.push(4, make_payload(4));
    jb.push(5, make_payload(5));

    // 窗口已满（late=1 >= 1）：本次 push 触发 drift rebase。
    // seq=6 的 diff = 6 - 2 = 4 <= R_TARGET → 小前跳，deadline 沿 cadence 推进 4 拍
    const auto before = jb.next_playout_deadline();
    jb.push(6, make_payload(6));
    const auto after = jb.next_playout_deadline();

    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(after.has_value());
    // 小缺口：deadline 精确推进 diff(4) × 10ms，而非 now+target 重新缓冲
    EXPECT_EQ(*after - *before, std::chrono::milliseconds(40));
}

TEST(JitterBufferTest, RebaseLargeGapRebuffersFromNow) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(102, make_payload(102));
    (void)jb.pop_next(out); // next_pop_seq_ = 101

    // diff = 500 - 101 = 399 >> TARGET=3：大断裂 → 重新缓冲（now + target×duration）
    jb.push(500, make_payload(500));
    const auto dl = jb.next_playout_deadline();
    ASSERT_TRUE(dl.has_value());
    const auto now = std::chrono::steady_clock::now();
    // 重新缓冲语义：deadline ≈ now + 30ms（40ms 上界容纳执行耗时）
    EXPECT_GT(*dl, now);
    EXPECT_LT(*dl - now, std::chrono::milliseconds(40));
}

TEST(JitterBufferTest, RebaseBackwardJumpKeepsCadenceNextBeat) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 正常播放 100-102，然后模拟源暂停：pop 空转 20 拍（next_pop_seq_ = 123）
    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(102, make_payload(102));
    for (int i = 0; i < 23; ++i) (void)jb.pop_next(out);

    // 源恢复：103.. 全部 late。前 7 个 late push 不改 deadline，
    // 第 CAPACITY(8) 个（seq=110）触发 consecutive-late rebase
    const auto before = jb.next_playout_deadline();
    for (std::uint32_t seq = 103; seq <= 110; ++seq) {
        jb.push(seq, make_payload(seq));
    }
    const auto after = jb.next_playout_deadline();

    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(after.has_value());
    // 回跳分支：deadline = D_old + 1 拍（10ms），下一拍播放 rebase 包
    EXPECT_EQ(*after - *before, std::chrono::milliseconds(10));
    EXPECT_EQ(jb.next_sequence(), 110u);
}

// ---- 自适应 target（Phase 2）----
// 小窗口配置（8 包/窗口，2 late 抬升，2 个干净窗口回落）保证测试确定性。
constexpr auto kAdaptCfg = aqua::jitter::AdaptiveTargetConfig {
    .window_packets = 8,
    .raise_late_count = 2,
    .lower_clean_windows = 2,
};

TEST(JitterBufferTest, AdaptiveTargetHoldsFloorWhenClean) {
    // 干净流量（push/pop 交替保持 diff 小）跨多个窗口：target 不许低于 floor
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, /*target=*/3, /*capacity=*/16,
                                  aqua::config::JITTER_DRIFT_WINDOW_PACKETS,
                                  aqua::config::JITTER_DRIFT_LATE_PACKET_THRESHOLD,
                                  kAdaptCfg);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 6 轮 push4/pop4 = 24 arrivals = 3 个满窗口，全部干净
    std::uint32_t seq = 0;
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < 4; ++i) jb.push(seq++, make_payload(seq - 1));
        for (int i = 0; i < 4; ++i) (void)jb.pop_next(out);
    }

    EXPECT_EQ(jb.target_latency_packets(), 3u); // floor 保持
    EXPECT_EQ(jb.rebases(), 0u);
    EXPECT_EQ(jb.late_packets(), 0u);
}

TEST(JitterBufferTest, AdaptiveTargetRaisesOnLatePressure) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, /*target=*/3, /*capacity=*/16,
                                  aqua::config::JITTER_DRIFT_WINDOW_PACKETS,
                                  aqua::config::JITTER_DRIFT_LATE_PACKET_THRESHOLD,
                                  kAdaptCfg);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 建立时间线并 pop 越过 seq 0..5（next_pop_seq_ = 6）
    jb.push(0, make_payload(0));
    for (int i = 0; i < 6; ++i) (void)jb.pop_next(out);

    // 2 个 late（seq 3,4 落后于 next_pop=6）+ 6 个 expected/future = 8 到达补满窗口
    jb.push(3, make_payload(3));   // late
    jb.push(4, make_payload(4));   // late
    for (std::uint32_t s = 6; s <= 11; ++s) jb.push(s, make_payload(s));

    // 窗口满：late=2 >= 2 → target 3→4
    EXPECT_EQ(jb.target_latency_packets(), 4u);

    // 第二轮压力窗口：pop 消费 6..9（next_pop=10），重推 6..9 → late×4，再补 4 个 expected
    for (int i = 0; i < 4; ++i) (void)jb.pop_next(out);
    for (std::uint32_t s = 6; s <= 9; ++s) jb.push(s, make_payload(s));   // 4 late（slot 已消费）
    for (std::uint32_t s = 12; s <= 15; ++s) jb.push(s, make_payload(s)); // 4 expected → 窗口满
    EXPECT_EQ(jb.target_latency_packets(), 5u); // 再次抬升
}

TEST(JitterBufferTest, AdaptiveTargetCappedAtHalfCapacity) {
    // target = capacity/2 = 4：抬升被上限挡住
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, /*target=*/4, /*capacity=*/8,
                                  aqua::config::JITTER_DRIFT_WINDOW_PACKETS,
                                  aqua::config::JITTER_DRIFT_LATE_PACKET_THRESHOLD,
                                  kAdaptCfg);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    jb.push(0, make_payload(0));
    for (int i = 0; i < 6; ++i) (void)jb.pop_next(out); // next_pop = 6

    // 5 个 late（seq 1..5 落后于 next_pop=6）+ 3 个 expected（6..8）= 8 到达补满窗口
    for (std::uint32_t s = 1; s <= 5; ++s) jb.push(s, make_payload(s));
    for (std::uint32_t s = 6; s <= 8; ++s) jb.push(s, make_payload(s));

    // late=5 >= 2 → 尝试抬升，但 target == capacity/2 被封顶挡住
    EXPECT_EQ(jb.target_latency_packets(), 4u);
}

TEST(JitterBufferTest, AdaptiveTargetRaisesToExplicitCeiling) {
    // 显式 ceiling 解除"天花板塌缩"：target=3、capacity=32（默认上限 capacity/2=16），
    // 但 max_packets=10 < 16 → 抬升到 10 后被显式上限挡住（证明 max 优先于 capacity/2）。
    auto cfg = kAdaptCfg;
    cfg.max_packets = 10;
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, /*target=*/3, /*capacity=*/32,
                                  aqua::config::JITTER_DRIFT_WINDOW_PACKETS,
                                  aqua::config::JITTER_DRIFT_LATE_PACKET_THRESHOLD,
                                  cfg);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 建立：push 0 后 pop 6 次（next_pop=6），窗口计数 0
    jb.push(0, make_payload(0));
    for (int i = 0; i < 6; ++i) (void)jb.pop_next(out);

    // 第一轮压力：2 late + 6 expected = 8 到达 → 窗口满 → raise 3→4
    jb.push(3, make_payload(3));   // late
    jb.push(4, make_payload(4));   // late
    for (std::uint32_t s = 6; s <= 11; ++s) jb.push(s, make_payload(s));
    ASSERT_EQ(jb.target_latency_packets(), 4u);

    // 通用压力轮（×6）：pop 4 消费 → 重推消费区 4 个 late → 补 4 个 expected
    // = 8 到达/轮 → 每轮一次 raise。target 4→5→...→10。
    for (std::uint32_t round = 0; round < 6; ++round) {
        for (int i = 0; i < 4; ++i) (void)jb.pop_next(out);
        const std::uint32_t base = 6 + 4 * round;          // 刚被消费的 4 个 seq
        for (std::uint32_t s = base; s < base + 4; ++s)    // late ×4（slot 已消费）
            jb.push(s, make_payload(s));
        for (std::uint32_t s = 12 + 4 * round; s < 16 + 4 * round; ++s) // expected ×4
            jb.push(s, make_payload(s));
    }
    EXPECT_EQ(jb.target_latency_packets(), 10u); // 到达显式 ceiling

    // 封顶后再来一轮压力：不再抬升
    for (int i = 0; i < 4; ++i) (void)jb.pop_next(out);
    const std::uint32_t base = 6 + 4 * 6;
    for (std::uint32_t s = base; s < base + 4; ++s) jb.push(s, make_payload(s));
    for (std::uint32_t s = 12 + 4 * 6; s < 16 + 4 * 6; ++s) jb.push(s, make_payload(s));
    EXPECT_EQ(jb.target_latency_packets(), 10u);
}

TEST(JitterBufferTest, AdaptiveTargetInvalidCeilingThrows) {
    auto cfg = kAdaptCfg;
    cfg.max_packets = 5; // > capacity/2 = 4
    EXPECT_THROW(
        aqua::jitter::JitterBuffer(make_test_format(), FRAMES_PER_PACKET, /*target=*/3, /*capacity=*/8,
                                   aqua::config::JITTER_DRIFT_WINDOW_PACKETS,
                                   aqua::config::JITTER_DRIFT_LATE_PACKET_THRESHOLD,
                                   cfg),
        std::invalid_argument);

    cfg.max_packets = 2; // < target = 3
    EXPECT_THROW(
        aqua::jitter::JitterBuffer(make_test_format(), FRAMES_PER_PACKET, /*target=*/3, /*capacity=*/16,
                                   aqua::config::JITTER_DRIFT_WINDOW_PACKETS,
                                   aqua::config::JITTER_DRIFT_LATE_PACKET_THRESHOLD,
                                   cfg),
        std::invalid_argument);
}

TEST(JitterBufferTest, AdaptiveTargetLowersAfterCleanWindows) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, /*target=*/3, /*capacity=*/16,
                                  aqua::config::JITTER_DRIFT_WINDOW_PACKETS,
                                  aqua::config::JITTER_DRIFT_LATE_PACKET_THRESHOLD,
                                  kAdaptCfg);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // Phase A: 抬升到 4（2 late + 6 expected 补满窗口）
    jb.push(0, make_payload(0));
    for (int i = 0; i < 6; ++i) (void)jb.pop_next(out); // next_pop = 6
    jb.push(3, make_payload(3));   // late
    jb.push(4, make_payload(4));   // late
    for (std::uint32_t s = 6; s <= 11; ++s) jb.push(s, make_payload(s)); // 窗口满 → raise
    ASSERT_EQ(jb.target_latency_packets(), 4u);

    // Phase B: 干净到达填 2 个窗口（每窗口 8）→ streak 2 → lower。
    // push2/pop2 交替控制 diff <= 7 < 16（不触发 jump rebase）。
    // 窗口2 在到达 8（push 19）满 → streak 1；窗口3 在第 16 个干净到达（push 27）满 → lower。
    for (std::uint32_t s = 12; s <= 26; ++s) {
        jb.push(s, make_payload(s));
        if (s % 2 == 1) {
            (void)jb.pop_next(out);
            (void)jb.pop_next(out);
        }
    }
    // 隔离断言：lower 发生在 push 27 内部，前后无 pop，deadline 前移恰 1 拍
    const auto before = jb.next_playout_deadline();
    jb.push(27, make_payload(27));
    const auto after = jb.next_playout_deadline();

    EXPECT_EQ(jb.target_latency_packets(), 3u); // 回落到 floor
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(*before - *after, std::chrono::milliseconds(10));
    EXPECT_EQ(jb.rebases(), 0u);
}
