#ifndef AQUA_RUNTIME_RUNTIME_CONFIG_H
#define AQUA_RUNTIME_RUNTIME_CONFIG_H

// runtime/CLI 层默认值与边界。统一放在 aqua::config，与 udp_config.h /
// grpc_config.h / buffer_config.h 共用同一命名空间，避免多个 config 命名空间
// 互相遮蔽。
//
// 分层：JB 相关的值一律**透传** buffer_config.h（Buffer 组件自己的默认），
// 本文件不复制第二份——同一个旋钮只在一处定义。

#include "aqua/audio/buffer/buffer_config.h"

#include <chrono>
#include <cstdint>

namespace aqua::config {

// gRPC 控制面默认端口（server 监听 / client 连接）。
inline constexpr std::uint16_t DEFAULT_RPC_PORT = 50051;
// UDP 数据面默认端口（server 监听；实际值由 server 通过 gRPC 下发给 client）。
inline constexpr std::uint16_t DEFAULT_UDP_PORT = 50000;
// server 默认绑定地址：监听所有 IPv4 接口。
inline constexpr char DEFAULT_BIND_IP[] = "0.0.0.0";
// 默认 client 显示名（仅用于 server 侧识别与日志）。
inline constexpr char DEFAULT_CLIENT_NAME[] = "aqua-client";

// client 抖动缓冲默认槽数（透传 Buffer 组件默认，= --jb-capacity 默认值）。
inline constexpr std::uint32_t DEFAULT_CLIENT_JB_CAPACITY_SLOTS = JB_DEFAULT_CAPACITY_SLOTS;
// server 采集→网络交接队列默认槽数（= --audio-queue-capacity 默认值）。
inline constexpr std::uint32_t DEFAULT_AUDIO_QUEUE_CAPACITY_SLOTS = 16;
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
// 显式指定 F（每包 sample frame 数）时的下限：再小则 RTP 头开销占比过高。
inline constexpr std::uint32_t MIN_FRAMES_PER_SLOT = 16;
// JB 容量合法区间（透传 Buffer 组件；下限是结构性的，上限纯护栏）。
inline constexpr std::uint32_t MIN_JB_CAPACITY_SLOTS = JB_MIN_CAPACITY_SLOTS;
inline constexpr std::uint32_t MAX_JB_CAPACITY_SLOTS = JB_MAX_CAPACITY_SLOTS;
inline constexpr std::uint32_t MAX_AUDIO_QUEUE_CAPACITY_SLOTS = 4096;
// 诊断快照与日志输出的节奏。
inline constexpr std::chrono::milliseconds DIAGNOSTICS_SNAPSHOT_INTERVAL { 1000 };

} // namespace aqua::config

#endif // AQUA_RUNTIME_RUNTIME_CONFIG_H
