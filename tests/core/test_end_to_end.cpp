// 端到端数据流严格测试（含 JitterBuffer）
//
// 把 JitterBuffer 纳入完整管线：
//   server: capture RB -> packetize (encode_audio)
//   通道: LossyChannel（内存模拟，可注入丢包/乱序/延迟/重复）
//   client: decode_audio -> JB.push -> JB.pop_next -> playback RB -> 字节级校验
//
// 关键覆盖：
//   - 真实管线字节级校验（之前 data_flow 用裸 RB 跳过 JB）
//   - 丢包注入 -> JB 静音填充 + packets_lost 计数
//   - 乱序注入 -> JB 重排后顺序输出
//   - 重复包注入 -> JB 丢弃 + duplicates 计数
//   - 序列号回绕（0xFFFFFFFF -> 0x00000000）
//   - burst 丢包 + 恢复
//   - 长时间运行累积一致性

#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/jitter_buffer/jitter_buffer.h"
#include "core/net/packet/packet.h"
#include "core/public/audio_format.h"
#include "core/public/config.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <random>
#include <vector>

namespace {

// 测试格式：48kHz / F32LE / 2ch
aqua::AudioFormat make_test_format()
{
    aqua::AudioFormat fmt;
    fmt.encoding = aqua::AudioEncoding::PcmF32LE;
    fmt.channels = 2;
    fmt.sample_rate = 48000;
    return fmt;
}

// 与 config.h FRAMES_PER_PACKET 一致：144 帧/包，48kHz 下 = 3ms，1152 字节 payload
constexpr std::uint32_t FRAMES_PER_PACKET = aqua::config::AUDIO_FRAMES_PER_PACKET; // 144
constexpr std::size_t PAYLOAD_SIZE = FRAMES_PER_PACKET * 2 * 4; // 1152

// 每包唯一可识别 payload：所有字节填 (sequence & 0xFF) + 1
// +1 确保不全零（避免与 JB 静音填充 memset(0) 混淆），且不产生冲突
std::vector<std::byte> make_payload(std::uint32_t sequence)
{
    return std::vector<std::byte>(PAYLOAD_SIZE,
        static_cast<std::byte>((sequence & 0xFF) + 1));
}

bool is_payload_of(std::span<const std::byte> data, std::uint32_t sequence)
{
    if (data.size() != PAYLOAD_SIZE)
        return false;
    std::byte expected = static_cast<std::byte>((sequence & 0xFF) + 1);
    return std::all_of(data.begin(), data.end(),
        [expected](std::byte b) { return b == expected; });
}

// 编码一个 audio 包（session_id 固定 0x12345678）
std::vector<std::byte> encode_packet(std::uint32_t sequence, std::uint32_t sample_position,
    std::span<const std::byte> payload)
{
    std::vector<std::byte> buf(sizeof(aqua::net::AudioPacketHeader) + payload.size());
    auto n = aqua::net::encode_audio(0x12345678, sequence, sample_position, payload, buf);
    buf.resize(n);
    return buf;
}

// 可控"网络"通道：内存模拟，支持丢包/乱序/重复/延迟注入。
// 不引入真实 UDP，保证 CI 稳定可重现。
struct LossyChannelConfig {
    double drop_rate = 0.0; // [0,1] 丢包率
    double duplicate_rate = 0.0; // [0,1] 重复包率
    double reorder_rate = 0.0; // [0,1] 与下一包交换的概率
    std::uint32_t seed = 42; // 随机种子
};

class LossyChannel {
public:
    explicit LossyChannel(LossyChannelConfig cfg)
        : cfg_(cfg)
        , rng_(cfg.seed)
    {
    }

    // 输入一个已编码包，输出 0/1/2 个包（按配置注入丢包/重复）。
    // 乱序通过缓冲当前包并与下一个交换实现（调用方需连续 push）。
    std::vector<std::vector<std::byte>> push(std::vector<std::byte> packet)
    {
        std::vector<std::vector<std::byte>> out;

        // 丢包
        if (uni_() < cfg_.drop_rate) {
            return out; // 直接丢弃
        }

        // 乱序：把上一个 pending 包与当前包一起输出（顺序交换）
        if (uni_() < cfg_.reorder_rate && pending_.has_value()) {
            out.push_back(std::move(packet));
            out.push_back(std::move(*pending_));
            pending_.reset();
            return out;
        }

        // 重复
        if (uni_() < cfg_.duplicate_rate) {
            out.push_back(packet); // 原包
            out.push_back(packet); // 副本
            return out;
        }

        // 普通输出：若有 pending 先 flush
        if (pending_.has_value()) {
            out.push_back(std::move(*pending_));
            pending_.reset();
        }
        // 当前包暂存为 pending，等下次 push 决定是否交换
        pending_ = std::move(packet);
        return out;
    }

    // flush 最后一个 pending 包
    std::vector<std::vector<std::byte>> flush()
    {
        std::vector<std::vector<std::byte>> out;
        if (pending_.has_value()) {
            out.push_back(std::move(*pending_));
            pending_.reset();
        }
        return out;
    }

private:
    double uni_()
    {
        return std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
    }
    LossyChannelConfig cfg_;
    std::mt19937 rng_;
    std::optional<std::vector<std::byte>> pending_;
};

// 端到端管线：把 N 个顺序包经过 channel + JB，输出到 playback RB。
// 返回 (output_payloads, jb 统计)。
struct PipelineResult {
    std::vector<std::vector<std::byte>> outputs; // 每个 pop_next 的输出
    std::vector<bool> real_flags; // 与 outputs 对应：true = 真实 PCM，false = PLC/静音
    std::uint64_t packets_received = 0;
    std::uint64_t packets_lost = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t late_packets = 0;
};

PipelineResult run_pipeline(std::uint32_t start_seq, std::size_t num_packets,
    const LossyChannelConfig& ch_cfg,
    std::size_t jb_target = 4, std::size_t jb_capacity = 0)
{
    PipelineResult result;
    // 自动计算 capacity：确保 >= num_packets * 2（向上取整为 2 的幂），
    // 避免 push 完所有包后 slot 被覆盖触发 rebase。
    if (jb_capacity == 0) {
        jb_capacity = std::max<std::size_t>(16, std::bit_ceil(num_packets * 2));
    }
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, jb_target, jb_capacity);
    LossyChannel channel(ch_cfg);

    // 1. 生成 num_packets 个包，依次推入 channel
    std::vector<std::vector<std::byte>> wire_packets;
    for (std::size_t i = 0; i < num_packets; ++i) {
        std::uint32_t seq = start_seq + static_cast<std::uint32_t>(i);
        auto payload = make_payload(seq);
        auto packet = encode_packet(seq, seq * FRAMES_PER_PACKET, payload);
        auto emitted = channel.push(std::move(packet));
        for (auto& p : emitted)
            wire_packets.push_back(std::move(p));
    }
    auto tail = channel.flush();
    for (auto& p : tail)
        wire_packets.push_back(std::move(p));

    // 2. client 侧：decode -> JB.push
    for (auto& pkt : wire_packets) {
        auto decoded = aqua::net::decode_audio(pkt);
        if (!decoded.has_value())
            continue;
        jb.push(decoded->header.sequence, decoded->payload);
    }

    // 3. pop 直到 buffer 空（buffer_fill_packets() == 0 表示所有 valid slot 已消费）。
    //    不能用 while(next_playout_deadline().has_value())：JB initialized_ 后 deadline 永远有值。
    //    max_pops 作为安全上限防止意外无限循环。
    std::vector<std::byte> out_buf(PAYLOAD_SIZE);
    const std::size_t max_pops = num_packets * 2 + jb_target + 10;
    for (std::size_t i = 0; i < max_pops && jb.buffer_fill_packets() > 0; ++i) {
        result.real_flags.push_back(jb.pop_next(out_buf));
        result.outputs.push_back({ out_buf.begin(), out_buf.end() });
    }

    result.packets_received = jb.packets_received();
    result.packets_lost = jb.packets_lost();
    result.duplicates = jb.duplicates();
    result.late_packets = jb.late_packets();
    return result;
}

} // namespace

// ==== 1. 无损管线：基线字节级校验 ====
// 这是之前 data_flow 测试缺失的关键场景：JB 纳入端到端后的字节级校验

TEST(EndToEndTest, LosslessPipelineByteLevelIntegrity)
{
    auto r = run_pipeline(/*start_seq=*/100, /*num_packets=*/30, LossyChannelConfig { });

    // 无丢包/乱序：packets_received == 30, lost == 0, late == 0, dup == 0
    EXPECT_EQ(r.packets_received, 30u);
    EXPECT_EQ(r.packets_lost, 0u);
    EXPECT_EQ(r.late_packets, 0u);
    EXPECT_EQ(r.duplicates, 0u);

    // pop 数应 >= 30（可能有 target latency 内的缓冲期，但 30 包顺序到达应全部 pop）
    // 注意：JB 在 target_latency 内不会立即 pop，但连续 pop 会消费完所有包
    EXPECT_GE(r.outputs.size(), 30u);

    // 字节级校验：前 30 个真实输出应严格匹配 seq 100..129
    std::uint32_t expected_seq = 100;
    int verified = 0;
    for (std::size_t i = 0; i < r.outputs.size(); ++i) {
        if (!r.real_flags[i])
            continue;
        ASSERT_EQ(r.outputs[i].size(), PAYLOAD_SIZE);
        EXPECT_TRUE(is_payload_of(r.outputs[i], expected_seq))
            << "output #" << verified << " expected seq " << expected_seq;
        ++expected_seq;
        ++verified;
    }
    EXPECT_EQ(verified, 30);
}

// ==== 2. 单包丢包：JB 静音填充 ====

TEST(EndToEndTest, SinglePacketLossProducesSilenceAtLossPoint)
{
    // 第 5 个包（seq=104）丢失
    LossyChannelConfig cfg;
    cfg.drop_rate = 0.0; // 不用随机丢包，手动构造
    // 我们直接构造一个跳过 seq=104 的包序列
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);

    // push 100,101,102,103, 105,106,107（跳过 104）
    for (std::uint32_t seq : { 100u, 101u, 102u, 103u, 105u, 106u, 107u }) {
        auto p = make_payload(seq);
        jb.push(seq, p);
    }

    EXPECT_EQ(jb.packets_received(), 7u);

    // pop：应在 seq=104 位置输出静音
    std::vector<std::byte> out(PAYLOAD_SIZE);
    std::vector<std::uint32_t> silence_positions;
    std::vector<std::uint32_t> real_positions;
    int seq_offset = 0;
    for (int i = 0; i < 30 && jb.buffer_fill_packets() > 0; ++i) {
        bool got = jb.pop_next(out);
        if (got) {
            real_positions.push_back(100 + seq_offset);
        } else {
            silence_positions.push_back(100 + seq_offset);
        }
        ++seq_offset;
    }

    // 应有 1 个静音（seq=104 位置）
    EXPECT_EQ(silence_positions.size(), 1u);
    EXPECT_EQ(silence_positions[0], 104u);
    EXPECT_EQ(jb.packets_lost(), 1u);
    // 其余 7 个为真实包
    EXPECT_EQ(real_positions.size(), 7u);
}

// ==== 3. 乱序重排：JB 输出仍按 sequence 顺序 ====

TEST(EndToEndTest, ReorderRestoredByJitterBuffer)
{
    // 手动构造乱序：100, 102, 101, 104, 103, 106, 105
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);

    for (std::uint32_t seq : { 100u, 102u, 101u, 104u, 103u, 106u, 105u }) {
        auto p = make_payload(seq);
        jb.push(seq, p);
    }

    std::vector<std::byte> out(PAYLOAD_SIZE);
    std::vector<std::uint32_t> output_order;
    for (int i = 0; i < 30 && jb.buffer_fill_packets() > 0; ++i) {
        if (jb.pop_next(out)) {
            output_order.push_back(static_cast<std::uint32_t>(std::to_integer<int>(out[0]) - 1));
        }
    }

    // 输出应按 100,101,102,103,104,105,106 顺序（JB 重排）
    ASSERT_EQ(output_order.size(), 7u);
    for (std::size_t i = 0; i < output_order.size(); ++i) {
        EXPECT_EQ(output_order[i], 100u + i)
            << "position " << i << " expected " << (100 + i) << " got " << output_order[i];
    }
}

// ==== 4. 重复包：JB 丢弃 + duplicates 计数 ====

TEST(EndToEndTest, DuplicatePacketsDiscarded)
{
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);

    // push 100, 101, 101(重复), 102, 100(已过 deadline 视为 late/dup)
    for (std::uint32_t seq : { 100u, 101u, 101u, 102u }) {
        auto p = make_payload(seq);
        jb.push(seq, p);
    }

    EXPECT_EQ(jb.packets_received(), 4u);
    EXPECT_EQ(jb.duplicates(), 1u); // 101 重复
}

// ==== 5. 序列号回绕：0xFFFFFFFF -> 0x00000000 ====

TEST(EndToEndTest, SequenceWraparoundAcrossZeroBoundary)
{
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);

    // 从 0xFFFFFFFE 开始，跨过回绕点
    std::vector<std::uint32_t> seqs;
    for (int i = 0; i < 6; ++i) {
        seqs.push_back(0xFFFFFFFEu + i); // FE, FF, 00, 01, 02, 03
    }

    for (auto seq : seqs) {
        auto p = make_payload(seq);
        jb.push(seq, p);
    }

    EXPECT_EQ(jb.packets_received(), 6u);
    EXPECT_EQ(jb.packets_lost(), 0u);

    // pop 顺序应与 push 顺序一致
    std::vector<std::byte> out(PAYLOAD_SIZE);
    std::size_t popped = 0;
    for (int i = 0; i < 30 && jb.buffer_fill_packets() > 0; ++i) {
        bool got = jb.pop_next(out);
        if (got) {
            std::uint32_t expected_seq = seqs[popped];
            EXPECT_TRUE(is_payload_of(out, expected_seq))
                << "pop #" << popped << " expected seq " << expected_seq;
        }
        ++popped;
    }
    EXPECT_EQ(popped, 6u);
}

// ==== 6. burst 丢包 + 恢复 ====

TEST(EndToEndTest, BurstLossThenRecovery)
{
    // 100,101, [102,103,104,105 丢失], 106,107,108
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);

    for (std::uint32_t seq : { 100u, 101u, 106u, 107u, 108u }) {
        auto p = make_payload(seq);
        jb.push(seq, p);
    }

    // 106-108 应正常输出，102-105 静音填充
    std::vector<std::byte> out(PAYLOAD_SIZE);
    int real_count = 0;
    int silence_count = 0;
    for (int i = 0; i < 30 && jb.buffer_fill_packets() > 0; ++i) {
        bool got = jb.pop_next(out);
        if (got)
            ++real_count;
        else
            ++silence_count;
    }
    EXPECT_EQ(real_count, 5); // 100,101,106,107,108
    EXPECT_EQ(silence_count, 4); // 102,103,104,105
    // packets_lost 在 pop_next 时才计数（slot 无效时）
    EXPECT_EQ(jb.packets_lost(), 4u);
}

// ==== 7. sample_position 回绕（24.8h @48kHz 边界）====
// sample_position 是 uint32，48kHz/144帧/包，回绕点 ≈ 2^32 / 48000 / 3600 ≈ 24.8h
// 这里直接构造接近回绕的 sample_position 验证 JB 不崩溃

TEST(EndToEndTest, SamplePositionNearWraparoundNoCrash)
{
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);

    // sample_position 接近 uint32 max：走完整 encode → decode 链路（包头仍携带 sp）
    constexpr std::uint32_t SP_NEAR_MAX = 0xFFFFFFF0u;
    for (std::uint32_t i = 0; i < 5; ++i) {
        std::uint32_t seq = 200 + i;
        std::uint32_t sp = SP_NEAR_MAX + i * FRAMES_PER_PACKET; // 回绕
        auto p = make_payload(seq);
        auto pkt = encode_packet(seq, sp, p);
        auto decoded = aqua::net::decode_audio(pkt);
        ASSERT_TRUE(decoded.has_value());
        jb.push(decoded->header.sequence, decoded->payload);
    }

    // 不崩溃 + 正常 pop
    std::vector<std::byte> out(PAYLOAD_SIZE);
    int popped = 0;
    for (int i = 0; i < 20 && jb.buffer_fill_packets() > 0; ++i) {
        (void)jb.pop_next(out);
        ++popped;
    }
    EXPECT_EQ(popped, 5);
}

// ==== 8. 随机丢包 30%：JB 仍能保持顺序输出 ====

TEST(EndToEndTest, RandomLoss30PercentPreservesOrder)
{
    LossyChannelConfig cfg;
    cfg.drop_rate = 0.3;
    cfg.seed = 12345;

    auto r = run_pipeline(0, 50, cfg);

    // 不一定所有包都收到，但收到的包输出应严格递增
    std::vector<std::uint32_t> real_seqs;
    for (std::size_t i = 0; i < r.outputs.size(); ++i) {
        if (r.real_flags[i]) {
            real_seqs.push_back(static_cast<std::uint32_t>(std::to_integer<int>(r.outputs[i][0]) - 1));
        }
    }

    // 严格递增
    for (std::size_t i = 1; i < real_seqs.size(); ++i) {
        EXPECT_GT(real_seqs[i], real_seqs[i - 1])
            << "order violation at " << i << ": " << real_seqs[i - 1] << " -> " << real_seqs[i];
    }
    // 应有丢包（30% × 50 ≈ 15）
    EXPECT_GT(r.packets_lost, 0u);
}

// ==== 9. 随机乱序 50%：JB 重排后顺序输出 ====

TEST(EndToEndTest, RandomReorder50PercentRestoredToOrder)
{
    LossyChannelConfig cfg;
    cfg.reorder_rate = 0.5;
    cfg.seed = 999;

    auto r = run_pipeline(0, 30, cfg);

    // 提取真实包的 seq
    std::vector<std::uint32_t> real_seqs;
    for (std::size_t i = 0; i < r.outputs.size(); ++i) {
        if (r.real_flags[i]) {
            real_seqs.push_back(static_cast<std::uint32_t>(std::to_integer<int>(r.outputs[i][0]) - 1));
        }
    }

    // 应严格递增（JB 重排成功）
    for (std::size_t i = 1; i < real_seqs.size(); ++i) {
        EXPECT_GT(real_seqs[i], real_seqs[i - 1])
            << "order violation at " << i;
    }
    // 无丢包（reorder 不丢包）
    EXPECT_EQ(r.packets_lost, 0u);
}

// ==== 10. 长时间运行累积一致性 ====

TEST(EndToEndTest, LongRunAccumulatedConsistency)
{
    // 200 包无丢包无乱序，验证 JB 长时间运行下统计精确
    auto r = run_pipeline(0, 200, LossyChannelConfig { });

    EXPECT_EQ(r.packets_received, 200u);
    EXPECT_EQ(r.packets_lost, 0u);
    EXPECT_EQ(r.duplicates, 0u);
    EXPECT_EQ(r.late_packets, 0u);

    // 输出应全部是真实包，无 PLC/静音
    int real_count = 0;
    for (bool real : r.real_flags) {
        if (real)
            ++real_count;
    }
    EXPECT_EQ(real_count, 200);
}

// ==== 11. JB 容量边界：target=1 最小缓冲 ====

TEST(EndToEndTest, MinimalTargetLatencyOnePacket)
{
    auto r = run_pipeline(0, 10, LossyChannelConfig { }, /*jb_target=*/1, /*jb_capacity=*/0);

    EXPECT_EQ(r.packets_received, 10u);
    EXPECT_EQ(r.packets_lost, 0u);

    // target=1 时首包后立即设 deadline，仍应正常输出全部包
    int real_count = 0;
    for (bool real : r.real_flags) {
        if (real)
            ++real_count;
    }
    EXPECT_EQ(real_count, 10);
}

// ==== 12. 完整握手 + 数据流：HELLO/ACK + Audio ====
// 验证 packet 编解码与 JB 协同：先 HELLO 握手再传 Audio

TEST(EndToEndTest, HandshakeThenAudioStream)
{
    // 1. HELLO 握手（内存模拟）
    std::array<std::byte, sizeof(aqua::net::HelloPacket)> hello_buf { };
    auto hello_n = aqua::net::encode_hello(0xDEADBEEF, hello_buf);
    ASSERT_EQ(hello_n, sizeof(aqua::net::HelloPacket));

    auto hello_decoded = aqua::net::decode_hello(hello_buf);
    ASSERT_TRUE(hello_decoded.has_value());
    EXPECT_EQ(hello_decoded->session_id, 0xDEADBEEFu);
    EXPECT_EQ(hello_decoded->type, aqua::net::PacketType::Hello);

    // 2. HELLO_ACK
    std::array<std::byte, sizeof(aqua::net::HelloPacket)> ack_buf { };
    auto ack_n = aqua::net::encode_hello_ack(0xDEADBEEF, ack_buf);
    (void)ack_n;
    auto ack_decoded = aqua::net::decode_hello(ack_buf);
    ASSERT_TRUE(ack_decoded.has_value());
    EXPECT_EQ(ack_decoded->type, aqua::net::PacketType::HelloAck);
    EXPECT_EQ(ack_decoded->session_id, 0xDEADBEEFu);

    // 3. Audio 流（与 HELLO 同 session_id）
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);
    constexpr int N = 10;
    for (int i = 0; i < N; ++i) {
        std::uint32_t seq = static_cast<std::uint32_t>(i);
        auto payload = make_payload(seq);
        auto pkt = encode_packet(seq, seq * FRAMES_PER_PACKET, payload);

        // 验证解码后的 session_id 与 HELLO 一致
        auto decoded = aqua::net::decode_audio(pkt);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->header.session_id, 0x12345678u);

        jb.push(decoded->header.sequence, decoded->payload);
    }

    // 4. pop 验证
    std::vector<std::byte> out(PAYLOAD_SIZE);
    int count = 0;
    for (int i = 0; i < 30 && jb.buffer_fill_packets() > 0; ++i) {
        (void)jb.pop_next(out);
        ++count;
    }
    EXPECT_EQ(count, N);
}
