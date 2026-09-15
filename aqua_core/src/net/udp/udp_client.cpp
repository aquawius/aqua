#include "aqua/net/udp/udp_client.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
#include "aqua/net/udp/network_frame.h"

#include <algorithm>

namespace aqua::net {

UdpClient::State::State(asio::io_context& ioc)
    : ioc(ioc)
    , strand(asio::make_strand(ioc))
    , transport(std::make_shared<UdpTransport>(ioc))
{
}

UdpClient::UdpClient(asio::io_context& ioc)
    : state_(std::make_shared<State>(ioc))
{
    log_debug("UdpClient created");
}

UdpClient::~UdpClient()
{
    stop();
}

std::expected<void, NetError> UdpClient::set_remote(const std::string& server_ip,
    std::uint16_t port)
{
    const auto st = state_;
    if (st->receive_started.load(std::memory_order_acquire)
        || st->heartbeat_started.load(std::memory_order_acquire)) {
        log_warn("UdpClient::set_remote ignored after data-plane startup");
        return std::unexpected(NetError::AlreadyStarted);
    }
    return st->transport->set_remote(server_ip, port);
}

std::expected<void, NetError> UdpClient::start_receive(std::size_t expected_payload_bytes,
    FrameHandler on_frame)
{
    if (expected_payload_bytes == 0 || !on_frame) {
        log_error("UdpClient::start_receive rejected: payload size must be non-zero and handler must be set");
        return std::unexpected(NetError::InvalidArgument);
    }
    const auto st = state_;

    // 在打开 socket 前先校验远端 endpoint。若先打开，即便本次调用即将失败，
    // 也会固定 socket 地址族（IPv4/IPv6），可能导致之后合法的 set_remote()
    // 选择到不兼容的地址族。
    const auto remote = st->transport->remote_endpoint();
    if (remote.port() == 0) {
        log_error("UdpClient::start_receive rejected: remote endpoint is not set");
        return std::unexpected(NetError::InvalidEndpoint);
    }

    if (!st->transport->is_open()) {
        if (const auto opened = st->transport->open(); !opened) {
            return opened;
        }
    }

    bool expected = false;
    if (!st->receive_started.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        log_warn("UdpClient::start_receive called twice, ignoring");
        return std::unexpected(NetError::AlreadyStarted);
    }
    auto handler = std::make_shared<FrameHandler>(std::move(on_frame));
    const std::weak_ptr<State> weak_st = st;
    const auto local_endpoint = st->transport->local_endpoint();
    log_debug_fmt("UdpClient receive configuration: local={} expected_payload={} remote={}",
        format_host_port(local_endpoint.address().to_string(), local_endpoint.port()),
        expected_payload_bytes,
        format_host_port(remote.address().to_string(), remote.port()));
    const auto started = st->transport->start_receive(
        [weak_st, expected_payload_bytes, handler](
            const asio::ip::udp::endpoint& sender, std::span<const std::byte> data) mutable {
            const auto st = weak_st.lock();
            if (!st) {
                return;
            }
            const auto frame = NetworkFrame::decode(data);
            if (!frame) {
                st->malformed_datagrams.fetch_add(1, std::memory_order_relaxed);
                log_trace_fmt("UdpClient ignored malformed datagram: bytes={}", data.size());
                return;
            }
            // Phase 0 arrival 观测：上报能通过 SSRC 钉住的 Audio 包。
            // 被钉住流检查丢弃的包（异 SSRC / ssrc==0）不上报：JitterEstimator
            // 见 ssrc 变化即 reset()，单个外来包足以把 J 清零并经 TargetController
            // 把 target 瞬间打落（欠载→惩罚抬升→震荡）。尺寸/endpoint 检查仍在
            // 下游做（观测的是网络到达，不关心 payload 几何），SSRC 门禁是
            // estimator 时间轴连续性的前提，与 JB 侧钉住同模型。
            if (frame->type() == PacketType::Audio && st->arrival_observer) {
                const auto pkt_ssrc = frame->ssrc();
                const bool ssrc_pinned = !st->rtp_ssrc_valid.load(std::memory_order_relaxed)
                    || pkt_ssrc == st->expected_rtp_ssrc.load(std::memory_order_relaxed);
                if (pkt_ssrc != 0 && ssrc_pinned) {
                    const auto arrival_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                                                .count();
                    try {
                        st->arrival_observer(frame->rtp_sequence(), frame->timestamp(),
                            frame->ssrc(), arrival_ns);
                    } catch (const std::exception& e) {
                        log_error_fmt("UdpClient arrival observer exception: {}",
                            format_exception_message(e));
                    } catch (...) {
                        log_error("UdpClient arrival observer unknown exception");
                    }
                }
            }
            if (frame->type() == PacketType::HeartbeatAck) {
                // HeartbeatAck 是 UDP endpoint discovery：只校验 session_id，不校验来源
                // 地址（IPv6 隐私扩展/多地址下，ACK 源可与 gRPC 通告地址不同）。
                // 通过即学习/刷新实际对端 endpoint，同时标记 association 建立。
                if (frame->session_id() == st->heartbeat_session_id.load(std::memory_order_acquire)
                    && frame->session_id() != 0) {
                    {
                        std::lock_guard lock(st->learned_mutex);
                        st->learned_peer_endpoint = sender;
                    }
                    st->heartbeat_ack_generation.fetch_add(1, std::memory_order_acq_rel);
                    st->heartbeat_ack_count.fetch_add(1, std::memory_order_relaxed);
                    if (aqua::log_level_enabled(aqua::LogLevel::Debug)) {
                        log_debug_fmt("UdpClient heartbeat ACK received: session=0x{:08X} endpoint={}",
                            frame->session_id(),
                            format_host_port(sender.address().to_string(), sender.port()));
                    }
                    // 首个有效 ACK = association 建立：定时器自动转稳态节奏
                    // （HEARTBEAT_INTERVAL 发 heartbeat + ACK 跟踪）；两阶段
                    // miss 记账同模型，阈值按 phase 取。
                    if (!st->associated.exchange(true, std::memory_order_acq_rel)) {
                        log_info_fmt("UdpClient association established: session=0x{:08X}, switching to heartbeat",
                            frame->session_id());
                    }
                    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                                            .count();
                    st->last_heartbeat_ack_ms.store(now_ms, std::memory_order_release);
                } else {
                    st->wrong_session_acks.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
            if (frame->type() != PacketType::Audio) {
                st->non_audio_datagrams.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // Audio 帧携带 SSRC 流身份 + RTP 序号，但不携带 session_id。
            // 两道约束（缺一即丢）：
            //   1. payload 尺寸（格式级，最先检查）；
            //   2. 来源 learned endpoint，或 SSRC 命中已钉住流（server 上游重定向：
            //      IPv6 临时地址轮换/网卡/VPN 抖动时源地址会变，重锁后继续接受）。
            if (frame->payload().size() != expected_payload_bytes) {
                st->audio_payload_mismatches.fetch_add(1, std::memory_order_relaxed);
                log_debug_fmt("UdpClient: dropping audio seq={} with payload={} bytes, expected={}",
                    frame->rtp_sequence(), frame->payload().size(), expected_payload_bytes);
                return;
            }
            {
                std::lock_guard lock(st->learned_mutex);
                const bool endpoint_ok = st->learned_peer_endpoint && sender == *st->learned_peer_endpoint;
                if (!endpoint_ok) {
                    const auto pkt_ssrc = frame->ssrc();
                    const bool stream_match = st->rtp_ssrc_valid.load(std::memory_order_relaxed)
                        && pkt_ssrc != 0
                        && pkt_ssrc
                            == st->expected_rtp_ssrc.load(std::memory_order_relaxed);
                    if (!stream_match) {
                        st->unexpected_sender_datagrams.fetch_add(1, std::memory_order_relaxed);
                        if (aqua::log_level_enabled(aqua::LogLevel::Debug)) {
                            log_debug_fmt("UdpClient ignored audio from unlearned sender: {}",
                                format_host_port(sender.address().to_string(), sender.port()));
                        }
                        return;
                    }
                    // 同一流换了上游地址：重锁 learned_peer_endpoint（建连时的
                    // endpoint 发现只做一次，这里是运行期续命）。
                    st->learned_peer_endpoint = sender;
                    if (aqua::log_level_enabled(aqua::LogLevel::Info)) {
                        log_info_fmt("UdpClient peer endpoint re-learned: {} (SSRC=0x{:08X} matched)",
                            format_host_port(sender.address().to_string(), sender.port()),
                            pkt_ssrc);
                    }
                }
            }
            // SSRC 流身份：首包钉住，之后不等即丢（与 learned_peer_endpoint 同模型；
            // timestamp 本阶段只解析不判定，不影响 JB）。
            const auto pkt_ssrc = frame->ssrc();
            if (pkt_ssrc == 0
                || (st->rtp_ssrc_valid.load(std::memory_order_relaxed)
                    && pkt_ssrc != st->expected_rtp_ssrc.load(std::memory_order_relaxed))) {
                st->malformed_datagrams.fetch_add(1, std::memory_order_relaxed);
                log_debug_fmt("UdpClient ignored audio with unexpected SSRC: got=0x{:08X}",
                    pkt_ssrc);
                return;
            }
            if (!st->rtp_ssrc_valid.load(std::memory_order_relaxed)) {
                st->expected_rtp_ssrc.store(pkt_ssrc, std::memory_order_relaxed);
                st->rtp_ssrc_valid.store(true, std::memory_order_relaxed);
            }
            // wire 16-bit → u64 extended sequence；下游（缺口统计/JB）语义不变。
            // 基准用"已见最大"而非"上次到达"：乱序/迟到包不得把基准写小，
            // 否则后到的正常包会被误判出 gap（100,99,101 会把 101 算成跳过 100）。
            const std::optional<std::uint64_t> last_ext = st->rtp_seq_valid.load(
                                                              std::memory_order_relaxed)
                ? std::optional<std::uint64_t> { st->last_rtp_ext_seq.load(std::memory_order_relaxed) }
                : std::nullopt;
            const auto ext_seq = extend_rtp_sequence(last_ext, frame->rtp_sequence());
            const bool is_new_max = !st->rtp_seq_valid.load(std::memory_order_relaxed)
                || ext_seq > st->last_rtp_ext_seq.load(std::memory_order_relaxed);
            if (is_new_max) {
                st->last_rtp_ext_seq.store(ext_seq, std::memory_order_relaxed);
                st->rtp_seq_valid.store(true, std::memory_order_relaxed);
            }
            log_trace_fmt("UdpClient audio frame accepted: seq={} bytes={}",
                ext_seq, frame->payload().size());
            if (*handler) {
                // 音频序列缺口统计（诊断）：只在"刷新已见最大"时判定 gap，
                // 乱序/重复包不触碰基准（与上面的 ext 基准同模型），否则诊断
                // gap 与 JB 实际缺口口径对不上（虚增）。
                const auto rx_seq = ext_seq;
                if (!st->rx_audio_seq_valid.load(std::memory_order_relaxed)) {
                    st->rx_audio_seq_valid.store(true, std::memory_order_relaxed);
                    st->last_rx_audio_seq.store(rx_seq, std::memory_order_relaxed);
                } else {
                    const auto rx_last = st->last_rx_audio_seq.load(std::memory_order_relaxed);
                    if (rx_seq > rx_last) {
                        if (rx_seq > rx_last + 1) {
                            st->rx_audio_gap_events.fetch_add(1, std::memory_order_relaxed);
                            st->rx_audio_missing_frames.fetch_add(
                                rx_seq - rx_last - 1, std::memory_order_relaxed);
                        }
                        st->last_rx_audio_seq.store(rx_seq, std::memory_order_relaxed);
                    }
                }
                (*handler)(ext_seq, frame->payload());
                st->audio_frames_accepted.fetch_add(1, std::memory_order_relaxed);
            }
        });
    if (!started) {
        st->receive_started.store(false, std::memory_order_release);
    }
    return started;
}

bool UdpClient::start_heartbeat(std::uint32_t session_id, std::chrono::milliseconds handshake_interval,
    LivenessHandler on_liveness_failure)
{
    const auto st = state_;
    if (session_id == 0) {
        log_error("UdpClient::start_heartbeat rejected: session_id is 0");
        return false;
    }
    if (handshake_interval <= std::chrono::milliseconds(0)) {
        log_error("UdpClient::start_heartbeat rejected: interval must be > 0");
        return false;
    }
    if (st->heartbeat_stopped.load(std::memory_order_acquire)) {
        return false;
    }
    if (!st->transport->has_remote()) {
        log_error("UdpClient::start_heartbeat rejected: remote endpoint is not set");
        return false;
    }
    bool expected_heartbeat_started = false;
    if (!st->heartbeat_started.compare_exchange_strong(expected_heartbeat_started, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        log_warn("UdpClient::start_heartbeat called twice, ignoring");
        return false;
    }
    try {
        asio::post(st->strand, [st, session_id, handshake_interval, on_liveness_failure = std::move(on_liveness_failure)]() mutable {
            try {
                if (st->heartbeat_stopped.load(std::memory_order_acquire)
                    || st->heartbeat_timer != nullptr) {
                    return;
                }
                if (!st->transport->has_remote()) {
                    log_error("UdpClient::start_heartbeat rejected: remote endpoint is not set");
                    return;
                }
                st->heartbeat_session_id.store(session_id, std::memory_order_release);
                st->handshake_interval = handshake_interval;
                st->heartbeat_ack_generation_seen = st->heartbeat_ack_generation.load(std::memory_order_acquire);
                st->heartbeat_ack_misses.store(0, std::memory_order_release);
                st->liveness_failed = false;
                st->on_liveness_failure = std::move(on_liveness_failure);
                st->heartbeat_ack_misses.store(0, std::memory_order_release);
                st->last_heartbeat_ack_ms.store(0, std::memory_order_release);
                st->heartbeat_timer = std::make_unique<asio::steady_timer>(st->strand);
                const auto local_endpoint = st->transport->local_endpoint();
                const auto remote_endpoint = st->transport->remote_endpoint();
                log_debug_fmt("UdpClient heartbeat configuration: session=0x{:08X} local={} remote={} handshake_interval={}ms handshake_miss_threshold={} steady_miss_threshold={}",
                    session_id,
                    format_host_port(local_endpoint.address().to_string(), local_endpoint.port()),
                    format_host_port(remote_endpoint.address().to_string(), remote_endpoint.port()),
                    handshake_interval.count(), config::HEARTBEAT_HANDSHAKE_ACK_MISS_THRESHOLD,
                    config::HEARTBEAT_ACK_MISS_THRESHOLD);
                const auto hb = NetworkFrame::heartbeat(
                    st->heartbeat_session_id.load(std::memory_order_acquire))
                                    .encode();
                st->transport->send(hb);
                st->last_tx_ms.store(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count(),
                    std::memory_order_release);
                st->heartbeat_handshake_send_attempts.fetch_add(1, std::memory_order_relaxed);
                log_debug_fmt("UdpClient initial heartbeat sent: session=0x{:08X}", session_id);
                log_trace_fmt("UdpClient heartbeat sent: session=0x{:08X}", session_id);
                schedule_beat(st);
            } catch (const std::exception& e) {
                log_error_fmt("UdpClient: failed to start heartbeat scheduler: {}", format_exception_message(e));
                st->heartbeat_failed.store(true, std::memory_order_release);
                st->heartbeat_stopped.store(true, std::memory_order_release);
                st->heartbeat_timer.reset();
            } catch (...) {
                log_error("UdpClient: failed to start heartbeat scheduler");
                st->heartbeat_failed.store(true, std::memory_order_release);
                st->heartbeat_stopped.store(true, std::memory_order_release);
                st->heartbeat_timer.reset();
            }
        });
        return true;
    } catch (const std::exception& e) {
        st->heartbeat_started.store(false, std::memory_order_release);
        log_error_fmt("UdpClient::start_heartbeat failed to schedule: {}", format_exception_message(e));
        return false;
    } catch (...) {
        st->heartbeat_started.store(false, std::memory_order_release);
        log_error("UdpClient::start_heartbeat failed to schedule");
        return false;
    }
}

void UdpClient::stop() noexcept
{
    const auto st = state_;
    log_debug("UdpClient stop requested");
    if (st->heartbeat_stopped.exchange(true, std::memory_order_acq_rel)) {
        st->transport->stop();
        return;
    }
    try {
        asio::post(st->strand, [st] {
            if (st->heartbeat_timer != nullptr) {
                asio::error_code ec;
                st->heartbeat_timer->cancel(ec);
                st->heartbeat_timer.reset();
            }
            log_debug("UdpClient heartbeat scheduler stopped on strand");
        });
    } catch (...) {
        // 若已无法再 post，State 会保活定时器直到所有待处理 handler/引用消失；
        // transport 的 stop 仍照常执行。
    }
    st->transport->stop();
}

void UdpClient::account_ack_miss(const std::shared_ptr<State>& state, std::uint32_t threshold)
{
    // miss 计数只有一份原子状态（strand 直接读写 relaxed，串行执行域保证
    // 顺序；外部诊断读同一原子，无需镜像同步）。
    if (state->heartbeat_ack_generation.load(std::memory_order_acquire)
        == state->heartbeat_ack_generation_seen) {
        const auto misses = state->heartbeat_ack_misses.fetch_add(1, std::memory_order_relaxed) + 1;
        state->heartbeat_ack_miss_events.fetch_add(1, std::memory_order_relaxed);
        log_trace_fmt("UdpClient heartbeat ACK miss: consecutive={} threshold={}", misses, threshold);
        if (!state->liveness_failed && misses >= threshold) {
            state->liveness_failed = true;
            // 对外锁存同步镜像：heartbeat_failed() 是 strand 外可见的唯一失败信号
            // （诊断快照与上层轮询都读它），miss 触发与调度异常在此汇合。
            state->heartbeat_failed.store(true, std::memory_order_release);
            if (state->on_liveness_failure) {
                try {
                    state->on_liveness_failure(misses);
                } catch (const std::exception& e) {
                    log_error_fmt("UdpClient liveness handler exception: {}", format_exception_message(e));
                } catch (...) {
                    log_error("UdpClient liveness handler unknown exception");
                }
            }
        }
    } else {
        state->heartbeat_ack_generation_seen = state->heartbeat_ack_generation.load(std::memory_order_acquire);
        state->heartbeat_ack_misses.store(0, std::memory_order_relaxed);
        log_trace("UdpClient heartbeat ACK observed; liveness miss counter reset");
    }
}

void UdpClient::schedule_beat(const std::shared_ptr<State>& state)
{
    if (state->heartbeat_timer == nullptr
        || state->heartbeat_stopped.load(std::memory_order_acquire)) {
        return;
    }
    // 单一定时器、节奏按 phase 定：未 association 用握手间隔发 heartbeat
    // （等 ACK 建连），已 association 用 HEARTBEAT_INTERVAL 发 heartbeat
    // 并做 ACK 跟踪。切换单向（association 建立后不再回握手期）。
    const bool associated_now = state->associated.load(std::memory_order_acquire);
    state->heartbeat_timer->expires_after(
        associated_now ? config::HEARTBEAT_INTERVAL : state->handshake_interval);
    const std::weak_ptr<State> weak_state = state;
    state->heartbeat_timer->async_wait(asio::bind_executor(state->strand,
        [weak_state](const asio::error_code& ec) {
            const auto state = weak_state.lock();
            if (!state || ec || state->heartbeat_stopped.load(std::memory_order_acquire)) {
                return;
            }

            // association 已建立：heartbeat 续命 + 路径探活。server 对每个合法
            // heartbeat 都回 ACK，本分支与握手期同模型记账：连续
            // HEARTBEAT_ACK_MISS_THRESHOLD 个周期无 ACK 即路径死亡。
            // activity-aware：距上次 client→server 发包不足一周期则跳过
            // （下游音频不抑制——下行包维持不了上行 NAT 映射；跳过的周期
            // 不发包也不计 miss，避免自证死亡）。
            if (state->associated.load(std::memory_order_acquire)) {
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                                        .count();
                if (now_ms - state->last_tx_ms.load(std::memory_order_acquire)
                    >= config::HEARTBEAT_INTERVAL.count()) {
                    account_ack_miss(state, config::HEARTBEAT_ACK_MISS_THRESHOLD);
                    try {
                        const auto hb = NetworkFrame::heartbeat(
                            state->heartbeat_session_id.load(std::memory_order_acquire))
                                            .encode();
                        state->transport->send(hb);
                        state->last_tx_ms.store(now_ms, std::memory_order_release);
                        log_trace_fmt("UdpClient heartbeat sent: session=0x{:08X}",
                            state->heartbeat_session_id.load(std::memory_order_relaxed));
                    } catch (const std::exception& e) {
                        // 发送失败不停止定时器：下周期重试（server 侧 5s 超时
                        // 才清理 session，本侧连续 miss 照常累积）。
                        log_debug_fmt("UdpClient heartbeat send failed: {}",
                            format_exception_message(e));
                    } catch (...) {
                        log_debug("UdpClient heartbeat send failed");
                    }
                }
                schedule_beat(state);
                return;
            }

            account_ack_miss(state, config::HEARTBEAT_HANDSHAKE_ACK_MISS_THRESHOLD);

            try {
                const auto hb = NetworkFrame::heartbeat(
                    state->heartbeat_session_id.load(std::memory_order_acquire))
                                    .encode();
                state->transport->send(hb);
                state->last_tx_ms.store(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count(),
                    std::memory_order_release);
                // 握手期每次发送都计数（heartbeat 期不经此处，不计数）。
                state->heartbeat_handshake_send_attempts.fetch_add(1, std::memory_order_relaxed);
                log_trace_fmt("UdpClient heartbeat sent: session=0x{:08X}",
                    state->heartbeat_session_id.load(std::memory_order_relaxed));
                schedule_beat(state);
            } catch (const std::exception& e) {
                log_error_fmt("UdpClient: heartbeat scheduling failed: {}", format_exception_message(e));
                state->heartbeat_failed.store(true, std::memory_order_release);
                state->heartbeat_stopped.store(true, std::memory_order_release);
                state->heartbeat_timer.reset();
            } catch (...) {
                log_error("UdpClient: heartbeat scheduling failed");
                state->heartbeat_failed.store(true, std::memory_order_release);
                state->heartbeat_stopped.store(true, std::memory_order_release);
                state->heartbeat_timer.reset();
            }
        }));
}

bool UdpClient::has_remote() const noexcept
{
    return state_->transport->has_remote();
}

void UdpClient::set_arrival_observer(ArrivalObserver observer)
{
    const auto st = state_;
    if (st->receive_started.load(std::memory_order_acquire)
        || st->heartbeat_started.load(std::memory_order_acquire)) {
        log_warn("UdpClient::set_arrival_observer ignored after data-plane startup");
        return;
    }
    st->arrival_observer = std::move(observer);
}
asio::ip::udp::endpoint UdpClient::remote_endpoint() const noexcept
{
    return state_->transport->remote_endpoint();
}

bool UdpClient::is_open() const noexcept
{
    return state_->transport->is_open();
}

asio::ip::udp::endpoint UdpClient::local_endpoint() const noexcept
{
    return state_->transport->local_endpoint();
}

UdpTransportStats UdpClient::stats() const noexcept
{
    return state_->transport->stats();
}

std::uint64_t UdpClient::heartbeat_ack_count() const noexcept
{
    return state_->heartbeat_ack_count.load(std::memory_order_relaxed);
}

std::uint32_t UdpClient::consecutive_heartbeat_ack_misses() const noexcept
{
    return state_->heartbeat_ack_misses.load(std::memory_order_acquire);
}

std::int64_t UdpClient::heartbeat_ack_age_ms() const noexcept
{
    const auto last = state_->last_heartbeat_ack_ms.load(std::memory_order_acquire);
    if (last == 0) {
        return -1;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    return std::max<std::int64_t>(0, now - last);
}

bool UdpClient::heartbeat_failed() const noexcept
{
    return state_->heartbeat_failed.load(std::memory_order_acquire);
}

std::uint64_t UdpClient::audio_frames_accepted() const noexcept { return state_->audio_frames_accepted.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::malformed_datagrams() const noexcept { return state_->malformed_datagrams.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::unexpected_sender_datagrams() const noexcept { return state_->unexpected_sender_datagrams.load(std::memory_order_relaxed); }
std::optional<asio::ip::udp::endpoint> UdpClient::learned_peer_endpoint() const noexcept
{
    const auto st = state_;
    std::lock_guard lock(st->learned_mutex);
    return st->learned_peer_endpoint;
}
std::uint64_t UdpClient::wrong_session_acks() const noexcept { return state_->wrong_session_acks.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::audio_payload_mismatches() const noexcept { return state_->audio_payload_mismatches.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::non_audio_datagrams() const noexcept { return state_->non_audio_datagrams.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::rx_audio_sequence_gap_events() const noexcept { return state_->rx_audio_gap_events.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::rx_audio_sequence_missing_frames() const noexcept { return state_->rx_audio_missing_frames.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::heartbeat_handshake_send_attempts() const noexcept { return state_->heartbeat_handshake_send_attempts.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::heartbeat_ack_miss_events() const noexcept { return state_->heartbeat_ack_miss_events.load(std::memory_order_relaxed); }

} // namespace aqua::net
