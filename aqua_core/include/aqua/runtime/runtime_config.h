#ifndef AQUA_RUNTIME_RUNTIME_CONFIG_H
#define AQUA_RUNTIME_RUNTIME_CONFIG_H

// runtime / CLI 层的默认值与边界。**只放没有别的模块可归的项**：
// server 绑定地址 + server 音频路径（capture -> 交接队列 -> dispatcher）的容量与 pacing。
//
// 分层约定（**同一个旋钮只有一个名字**）：
//   - 端口 / 协议默认值归各自的传输模块，本文件不再转定义：
//       DEFAULT_RPC_PORT / DEFAULT_CLIENT_NAME  -> net/grpc/grpc_config.h
//       DEFAULT_UDP_PORT / MIN_FRAMES_PER_SLOT   -> net/udp/udp_config.h
//   - Buffer 组件（JB_*）的默认值一律在 audio/buffer/buffer_config.h。
//     本文件**不为它们起第二个名字**：历史上的
//     DEFAULT_CLIENT_JB_CAPACITY_SLOTS / MIN_JB_CAPACITY_SLOTS / MAX_JB_CAPACITY_SLOTS
//     分别是 JB_DEFAULT_CAPACITY_SLOTS / JB_MIN_CAPACITY_SLOTS /
//     JB_MAX_CAPACITY_SLOTS 的别名，已删除；用到它们的地方直接引用 buffer_config.h 的名字。
//   - 诊断节奏 -> diagnostics/diagnostics_config.h；
//   - control poll 节奏 -> runtime/runtime_state.h。

#include <chrono>
#include <cstdint>

namespace aqua::config {

// server 默认绑定地址：监听所有 IPv4 接口。gRPC 与 UDP 共用；
// 两者的默认端口分别见 grpc_config.h / udp_config.h。
inline constexpr char DEFAULT_BIND_IP[] = "0.0.0.0";

// server 采集→网络交接队列默认槽数（= --audio-queue-capacity 默认值）。
// 实测 48 比原 16 更稳：pacing 追赶缓冲更深，能把 WASAPI 采集回调每 10ms
// 成簇交出的 2~3 包摊平成稳定流，且不增加稳态延迟（队列平时近空）。仅容量变更。
inline constexpr std::uint32_t DEFAULT_AUDIO_QUEUE_CAPACITY_SLOTS = 48;
// dispatcher 发包 pacing 的追赶深度（槽）：交接队列积压达到该深度时进入追赶
// 模式（加速排空）。必须明显大于自然摆动深度——capture 周期成串 2~3 包、叠加
// worker 被调度延迟 1~2 包，队列深度在 0..5 间摆动；阈值 4 实测（Wi-Fi，
// F=175/3.65ms）每秒触发 ~20 次追平，pacing 名存实亡。8 槽 ≈ 29ms 积压才进入
// 追赶：自然摆动不会误入，真饿死也能快速恢复。
inline constexpr std::uint32_t DISPATCH_PACING_CATCHUP_DEPTH_SLOTS = 8;
// 追赶模式的排空倍速：积压超阈值后按 interval/SPEEDUP 的间隔继续逐包发送
// （净排空速率 = (SPEEDUP-1) × 正常速率）。刻意**不**用"一次性全发"：整批
// 背靠背打出只是把突发从发送端搬到接收端，会把接收端抖动估计瞬间抬高——
// 与 pacing 的设计意图自相矛盾。
inline constexpr std::uint32_t DISPATCH_PACING_CATCHUP_SPEEDUP = 3;
// pacing 绝对时刻表的欠账补发上限（包）：worker 唤醒延迟超过一个 packet 间隔
// 时，当拍最多连续补发的包数，把时刻表追平。绝对排表（next_send += interval）
// 下 <一个间隔的唤醒延迟不累积（相对排表 = now+interval 会把每拍 ~1ms 的调度
// 延迟累积成系统性降速，实测笔记本上发送速率掉到 ~220/s、队列均值 4 槽）；
// 欠账上限防止长期队列空转把"时刻表信用"攒成后来的大突发。
inline constexpr std::uint32_t DISPATCH_PACING_MAX_CATCHUP_SENDS = 2;
// 交接队列容量的下限：必须大于 pacing 追赶深度，否则队列会先于追赶触发而
// 丢帧（队列溢出语义是丢最新帧），追赶路径永远走不到、积压也无法被排空。
inline constexpr std::uint32_t MIN_AUDIO_QUEUE_CAPACITY_SLOTS
    = DISPATCH_PACING_CATCHUP_DEPTH_SLOTS + 1;
// 交接队列容量上限（纯护栏）。
inline constexpr std::uint32_t MAX_AUDIO_QUEUE_CAPACITY_SLOTS = 4096;

} // namespace aqua::config

#endif // AQUA_RUNTIME_RUNTIME_CONFIG_H
