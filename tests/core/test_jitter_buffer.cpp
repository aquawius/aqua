#include "core/jitter_buffer/jitter_buffer.h"
#include "core/public/audio_format.h"

#include <gtest/gtest.h>

#include <algorithm>
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

// push 并检测是否触发了 rebase。
// rebase (init_timeline) 会重置 next_deadline_；普通 push 不修改 deadline。
// 不能用 next_sequence() 比较：当 rebase 发生在 expected 包上时（diff=0），
// next_pop_seq_ 已等于 seq，init_timeline 不会改变它。
bool push_and_check_rebase(aqua::jitter::JitterBuffer& jb, std::uint32_t seq) {
    auto payload = make_payload(seq);
    auto deadline_before = jb.next_playout_deadline();
    jb.push(seq, seq * FRAMES_PER_PACKET, payload);
    auto deadline_after = jb.next_playout_deadline();
    // 首包 init_timeline: deadline 从 nullopt → value（不在此函数检测范围）
    // rebase init_timeline: deadline 从旧值 → 新值（基于不同 now()，必定不同）
    // 普通 push: deadline 不变
    return deadline_before.has_value() && deadline_after.has_value()
        && *deadline_before != *deadline_after;
}

} // namespace

// ---- 基本功能 ----

TEST(JitterBufferTest, NormalInOrderPlayback) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 3 包
    for (std::uint32_t seq = 100; seq < 103; ++seq) {
        auto payload = make_payload(seq);
        jb.push(seq, seq * 480, payload);
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
    jb.push(100, 0, make_payload(100));

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
    jb.push(100, 0, make_payload(100));
    jb.push(102, 960, make_payload(102));
    jb.push(101, 480, make_payload(101));
    jb.push(103, 1440, make_payload(103));

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

TEST(JitterBufferTest, SinglePacketLossProducesSilence) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 100, 101, 103（缺 102）
    jb.push(100, 0, make_payload(100));
    jb.push(101, 480, make_payload(101));
    jb.push(103, 1440, make_payload(103));

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // pop: 100, 101 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 100));
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 101));

    // 102 应该是静音（丢包）
    EXPECT_FALSE(jb.pop_next(out));
    EXPECT_TRUE(is_silence(out));

    // 103 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 103));

    EXPECT_EQ(jb.packets_lost(), 1);
}

TEST(JitterBufferTest, ConsecutivePacketLossProducesSilence) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 100, 101, 105（缺 102, 103, 104）
    jb.push(100, 0, make_payload(100));
    jb.push(101, 480, make_payload(101));
    jb.push(105, 2400, make_payload(105));

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // 100, 101 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 100));
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 101));

    // 102, 103, 104 静音
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(jb.pop_next(out));
        EXPECT_TRUE(is_silence(out));
    }

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
    jb.push(100, 0, make_payload(100));
    jb.push(101, 480, make_payload(101));
    jb.push(101, 480, make_payload(101));  // 重复
    jb.push(102, 960, make_payload(102));

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
    jb.push(100, 0, make_payload(100));
    jb.push(101, 480, make_payload(101));
    jb.push(103, 1440, make_payload(103));

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // pop 100 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 100));

    // 在 102 的 deadline 之前 push 102
    jb.push(102, 960, make_payload(102));

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
    jb.push(100, 0, make_payload(100));
    jb.push(101, 480, make_payload(101));
    jb.push(103, 1440, make_payload(103));

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // pop 100, 101 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 100));
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 101));

    // pop 102 → 静音（102 已丢）
    EXPECT_FALSE(jb.pop_next(out));
    EXPECT_TRUE(is_silence(out));

    // 103 正常
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 103));

    // 现在 push 102 → 应该被判定为 late（因为 next_pop_seq_ 已经是 104）
    jb.push(102, 960, make_payload(102));

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
        jb.push(seq, seq * 480, make_payload(seq));
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
    jb.push(100, 0, make_payload(100));
    jb.push(101, 480, make_payload(101));
    jb.push(102, 960, make_payload(102));

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    // pop 100
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 100));

    // 巨大跳跃：push 500（远超 capacity=8）
    jb.push(500, 0, make_payload(500));
    jb.push(501, 480, make_payload(501));
    jb.push(502, 960, make_payload(502));

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
    jb.push(100, 0, make_payload(100));
    jb.push(101, 480, make_payload(101));
    jb.push(102, 960, make_payload(102));
    jb.push(103, 1440, make_payload(103));

    // pop 100-101，next_pop_seq_ = 102
    (void)jb.pop_next(out);  // 100
    (void)jb.pop_next(out);  // 101

    // 巨大跳跃：push 110（diff = 110 - 102 = 8 >= capacity=8）
    // 软 rebase 应将 next_pop_seq_ 跳到 110，但不清除 slot。
    // 102 和 103 仍在 slot 中，但它们的 seq < 110（新 next_pop_seq_），
    // 所以 pop 时会被跳过（静音填充）。
    jb.push(110, 0, make_payload(110));

    // pop 应返回 110（rebase 基准包）
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, 110));

    // 111-112 未推送，应静音
    EXPECT_FALSE(jb.pop_next(out));  // 111: silence
    EXPECT_TRUE(is_silence(out));
    EXPECT_FALSE(jb.pop_next(out));  // 112: silence
    EXPECT_TRUE(is_silence(out));

    // 统计应保留
    EXPECT_GE(jb.packets_received(), 5);
    EXPECT_GT(jb.packets_lost(), 0);  // 102-109, 111-112 等丢失
}

// ---- payload 大小校验 ----

TEST(JitterBufferTest, PayloadSizeMismatchIgnored) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);

    // push 一个大小不匹配的 payload
    std::vector<std::byte> wrong_size(PAYLOAD_SIZE / 2);
    jb.push(100, 0, wrong_size);

    // 不应该被接收
    EXPECT_EQ(jb.packets_received(), 0);
    EXPECT_FALSE(jb.next_playout_deadline().has_value());
}

// ---- 连续正常运行 ----

TEST(JitterBufferTest, ContinuousNormalOperation) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // 模拟 100 个包的连续收发
    // 初始缓冲 3 包，然后每收到 1 包就 pop 1 包
    for (std::uint32_t seq = 0; seq < 3; ++seq) {
        jb.push(seq, seq * 480, make_payload(seq));
    }

    EXPECT_TRUE(jb.next_playout_deadline().has_value());

    for (std::uint32_t seq = 0; seq < 100; ++seq) {
        // pop 当前包
        EXPECT_TRUE(jb.pop_next(out)) << "Failed at seq " << seq;
        EXPECT_TRUE(is_payload_of(out, seq)) << "Wrong payload at seq " << seq;

        // push 下一个包（如果还有）
        std::uint32_t next_push = seq + 3;
        if (next_push < 100) {
            jb.push(next_push, next_push * 480, make_payload(next_push));
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
    jb.push(100, 0, make_payload(100));
    jb.push(101, 480, make_payload(101));
    jb.push(102, 960, make_payload(102));

    // 应有 3 包
    EXPECT_EQ(jb.buffer_fill_packets(), 3);
}

// ---- reset 测试 ----

TEST(JitterBufferTest, ResetClearsPlayoutStateButNotStatistics) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 3 包 + pop 1 包（制造一些统计）
    jb.push(100, 0, make_payload(100));
    jb.push(101, 480, make_payload(101));
    jb.push(102, 960, make_payload(102));
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
    jb.push(200, 0, make_payload(200));
    EXPECT_EQ(jb.packets_received(), 4);
    EXPECT_EQ(jb.next_sequence(), 200);
    EXPECT_TRUE(jb.next_playout_deadline().has_value());
}

// ---- next_sequence 返回值 ----

TEST(JitterBufferTest, NextSequenceAfterPush) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);

    // push 1 包
    jb.push(500, 0, make_payload(500));
    EXPECT_EQ(jb.next_sequence(), 500);

    std::vector<std::byte> out(PAYLOAD_SIZE);
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_EQ(jb.next_sequence(), 501);
}

// ---- pop 输出大小不足时返回 false ----

TEST(JitterBufferTest, PopOutputBufferTooSmall) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, TARGET, CAPACITY);

    // push 1 包
    jb.push(0, 0, make_payload(0));

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
    jb.push(100, 0, make_payload(100));
    jb.push(101, 480, make_payload(101));
    jb.push(102, 960, make_payload(102));

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
        jb.push(seq, (seq - 100) * FRAMES_PER_PACKET, make_payload(seq));
    }
    // 第 CAPACITY 个 late 包应触发 reset，以最后到达的包重建时间线
    // reset 后 next_sequence 应等于触发 reset 的那个包的 seq
    std::uint32_t expected_reset_seq = 103 + CAPACITY - 1;  // 110
    EXPECT_EQ(jb.next_sequence(), expected_reset_seq);

    // reset 后应能正常播放触发 reset 的包
    EXPECT_TRUE(jb.pop_next(out));
    EXPECT_TRUE(is_payload_of(out, expected_reset_seq));

    // 后续包应正常工作（push 111 后 pop）
    jb.push(111, 0, make_payload(111));
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
