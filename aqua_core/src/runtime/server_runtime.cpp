#include "aqua/runtime/server_runtime.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
#include "aqua/net/net_error.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <random>
#include <system_error>

namespace aqua::runtime {
namespace {

    [[nodiscard]] audio::AudioDeviceDirection capture_direction(audio::AudioCaptureSource source) noexcept
    {
        switch (source) {
        case audio::AudioCaptureSource::INPUT_DEVICE:
            return audio::AudioDeviceDirection::INPUT;
        case audio::AudioCaptureSource::OUTPUT_LOOPBACK:
            return audio::AudioDeviceDirection::OUTPUT;
        }
        return audio::AudioDeviceDirection::NONE;
    }

    [[nodiscard]] std::optional<audio::AudioDeviceId> resolve_effective_capture_device(
        const ServerRuntimeConfig& config,
        const audio::AudioDeviceManager* device_mgr)
    {
        if (device_mgr == nullptr) {
            return std::nullopt;
        }
        const auto direction = capture_direction(config.capture.source);
        if (direction == audio::AudioDeviceDirection::NONE) {
            return std::nullopt;
        }
        const auto resolved = device_mgr->resolve(direction, config.capture.device);
        if (!resolved) {
            return std::nullopt;
        }
        return resolved->id;
    }

    [[nodiscard]] audio::AudioFormat resolve_effective_format(
        const ServerRuntimeConfig& config,
        const audio::AudioDeviceManager* device_mgr,
        const std::optional<audio::AudioDeviceId>& effective_device)
    {
        if (config.format) {
            return *config.format;
        }
        if (device_mgr == nullptr || !effective_device) {
            return { };
        }
        const auto direction = capture_direction(config.capture.source);
        if (direction == audio::AudioDeviceDirection::NONE) {
            return { };
        }
        const auto result = device_mgr->default_format(direction, effective_device);
        return result ? *result : audio::AudioFormat { };
    }

    [[nodiscard]] std::uint32_t resolve_effective_frame_count(
        std::uint32_t requested, const audio::AudioFormat& format) noexcept
    {
        if (!format.is_valid()) {
            return 0;
        }
        std::uint32_t frames = 0;
        if (requested == 0) {
            // auto-F：MTU 预算与包时长上限取小（时长上限的取值理由见
            // udp_config.h UDP_AUDIO_MAX_PACKET_MS）。
            const auto budget_frames = audio::frame_count_for_budget(
                format, aqua::config::UDP_AUDIO_PAYLOAD_BYTES);
            const auto duration_frames = audio::frame_count_for_duration(
                format, aqua::config::UDP_AUDIO_MAX_PACKET_MS);
            if (budget_frames == 0 || duration_frames == 0) {
                return 0;
            }
            frames = std::min(budget_frames, duration_frames);
        } else {
            if (requested < config::MIN_FRAMES_PER_SLOT) {
                return 0;
            }
            const auto bytes = format.bytes_for_frames(requested);
            if (bytes == 0 || bytes > aqua::config::UDP_AUDIO_PAYLOAD_BYTES) {
                return 0;
            }
            frames = requested;
        }
        // 包时长下界（UDP_AUDIO_MIN_PACKET_MS）：多声道 + 高位深 + 高采样率的
        // 极端格式下 frame_bytes 很大，MTU 预算只能给出极小的 F，包率会高到
        // pacing 无法实现（间隔低于可实现精度）、且交接队列按槽计只有几毫秒音频
        // 而采集每 10ms 交付数十包 —— 必然持续溢出丢帧。这类格式在 1400B payload
        // 预算下根本无法可靠传输（满足下界所需的 F 会超出预算），故启动期直接
        // 拒绝，而不是静默跑起来疯狂丢帧。
        if (format.sample_rate == 0) {
            return 0;
        }
        const double packet_ms
            = static_cast<double>(frames) * 1000.0 / static_cast<double>(format.sample_rate);
        if (packet_ms < aqua::config::UDP_AUDIO_MIN_PACKET_MS) {
            log_warn_fmt(
                "ServerRuntime: packet duration {:.3f}ms (F={} rate={}) below the {}ms floor "
                "- the format cannot be carried at the {}B payload budget; reduce channels/"
                "bit depth/sample rate or raise --audio-packet-frames explicitly",
                packet_ms, frames, format.sample_rate,
                aqua::config::UDP_AUDIO_MIN_PACKET_MS,
                aqua::config::UDP_AUDIO_PAYLOAD_BYTES);
            return 0;
        }
        return frames;
    }

} // namespace

ServerRuntime::ServerRuntime(asio::io_context& ioc, const ServerRuntimeConfig& config)
    : config_(config)
    , ioc_(ioc)
    , device_mgr_(audio::create_device_manager())
    , effective_capture_device_(resolve_effective_capture_device(config_, device_mgr_.get()))
    , effective_format_(resolve_effective_format(config_, device_mgr_.get(), effective_capture_device_))
    , effective_frame_count_(resolve_effective_frame_count(config_.frame_count, effective_format_))
    // 容量下限 MIN_AUDIO_QUEUE_CAPACITY_SLOTS（> pacing 追赶深度）：小于它时队列
    // 会先于追赶触发而丢最新帧，积压永远排不空、pacing 名存实亡。
    , effective_audio_queue_capacity_slots_(config_.audio_queue_capacity_slots
                      >= config::MIN_AUDIO_QUEUE_CAPACITY_SLOTS
                  && config_.audio_queue_capacity_slots <= config::MAX_AUDIO_QUEUE_CAPACITY_SLOTS
              ? config_.audio_queue_capacity_slots
              : 0) // 0 = 非法标记，start() 据此直接拒绝
    , sessions_(std::make_shared<session::SessionManager>())
    , udp_(ioc, sessions_)
    , packetizer_(effective_frame_count_, effective_format_.frame_bytes())
    , frame_queue_(effective_audio_queue_capacity_slots_, effective_frame_count_, effective_format_.frame_bytes())
    , dispatcher_(frame_queue_, udp_)
{
    // RTP 流身份：ssrc 非零（0 留作"未定"哨兵，client 首包钉住后不再接受 0）；
    // timestamp 首帧偏移随机（RFC 3550，避免初值污染接收端延迟估计）。
    // thread_local mt19937_64 与 SessionManager 同源（random_device 播种一次）。
    thread_local std::mt19937_64 rng { [] {
        std::random_device rd;
        return static_cast<std::uint64_t>(rd()) << 32 | rd();
    }() };
    do {
        rtp_ssrc_ = static_cast<std::uint32_t>(rng());
    } while (rtp_ssrc_ == 0);
    rtp_timestamp_offset_ = static_cast<std::uint32_t>(rng());
    dispatcher_.set_rtp_params(rtp_ssrc_, rtp_timestamp_offset_);
    // 发包 pacing：把 capture 周期的成串发包摊平到 packet 周期（F/sample_rate）上。
    // 接收端 J（RFC 3550 均值型）不再被确定性 burst 撑大，自适应 target 得以贴近
    // 几何地板——低延迟链路的关键杠杆。格式非法（F=0）时间隔为 0 = 关闭，
    // start() 会在那之前拒绝。
    std::chrono::nanoseconds pacing_interval { 0 };
    if (effective_frame_count_ > 0 && effective_format_.sample_rate > 0) {
        pacing_interval = std::chrono::nanoseconds(
            static_cast<std::uint64_t>(effective_frame_count_) * 1'000'000'000ULL
            / effective_format_.sample_rate);
    }
    dispatcher_.set_pacing(pacing_interval);
    log_debug_fmt("ServerRuntime instance created: audio_queue_capacity_slots={} packet_frames={} frame_bytes={} rtp_ssrc=0x{:08X} pacing_ns={}",
        config_.audio_queue_capacity_slots, effective_frame_count_, effective_format_.frame_bytes(),
        rtp_ssrc_, pacing_interval.count());
}

ServerRuntime::~ServerRuntime()
{
    stop();
}

bool ServerRuntime::enter_starting() noexcept
{
    RuntimeState expected = RuntimeState::Created;
    const bool entered = state_.compare_exchange_strong(expected, RuntimeState::Starting,
        std::memory_order_acq_rel, std::memory_order_acquire);
    if (entered) {
        log_debug("ServerRuntime state: Created -> Starting");
    }
    return entered;
}

bool ServerRuntime::enter_stopping() noexcept
{
    auto state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == RuntimeState::Stopping || state == RuntimeState::Stopped) {
            return false;
        }
        if (state_.compare_exchange_weak(state, RuntimeState::Stopping,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            log_debug("ServerRuntime state: -> Stopping");
            return true;
        }
    }
}

void ServerRuntime::enter_stopped() noexcept
{
    state_.store(RuntimeState::Stopped, std::memory_order_release);
    log_debug("ServerRuntime state: -> Stopped");
}

bool ServerRuntime::start()
{
    std::lock_guard lock(lifecycle_mutex_);
    if (!enter_starting()) {
        return false;
    }
    if (weak_from_this().expired()) {
        log_error("ServerRuntime must be owned by std::shared_ptr");
        stop_locked();
        return false;
    }

    const auto frame_bytes = effective_format_.frame_bytes();
    // Validate user/configuration-facing invariants individually so the first visible
    // startup failure always carries an actionable reason at Error level.
    if (!effective_format_.is_valid()) {
        if (config_.format) {
            log_error("ServerRuntime: invalid server audio format");
        } else {
            log_error("ServerRuntime: backend did not provide a usable default capture format");
        }
        stop_locked();
        return false;
    }
    if (effective_frame_count_ < config::MIN_FRAMES_PER_SLOT) {
        log_error_fmt("ServerRuntime: frame_count must be auto-resolved or >= {} and fit within UDP payload budget", config::MIN_FRAMES_PER_SLOT);
        stop_locked();
        return false;
    }
    if (effective_audio_queue_capacity_slots_ == 0) {
        log_error_fmt("ServerRuntime: audio_queue_capacity_slots must be {}..{}", config::MIN_AUDIO_QUEUE_CAPACITY_SLOTS, config::MAX_AUDIO_QUEUE_CAPACITY_SLOTS);
        stop_locked();
        return false;
    }
    if (config_.session_timeout <= std::chrono::milliseconds(0)) {
        log_error_fmt("ServerRuntime: session_timeout={}ms must be > 0", config_.session_timeout.count());
        stop_locked();
        return false;
    }
    if (config_.session_reap_interval <= std::chrono::milliseconds(0)) {
        log_error_fmt("ServerRuntime: session_reap_interval={}ms must be > 0", config_.session_reap_interval.count());
        stop_locked();
        return false;
    }
    if (config_.rpc_port == 0) {
        log_error("ServerRuntime: rpc_port must be > 0");
        stop_locked();
        return false;
    }
    if (!packetizer_.valid() || !frame_queue_.valid() || frame_bytes == 0) {
        log_error("ServerRuntime: invalid audio/network queue geometry");
        stop_locked();
        return false;
    }
    if (static_cast<std::size_t>(effective_frame_count_)
        > std::numeric_limits<std::size_t>::max() / frame_bytes) {
        log_error("ServerRuntime: frame_count × frame_bytes overflows size_t");
        stop_locked();
        return false;
    }

    const std::string effective_advertised_udp_address = config_.advertised_udp_address.empty() ? config_.server_ip : config_.advertised_udp_address;
    try {
        (void)::aqua::net::parse_ip_address(config_.server_ip);
        (void)::aqua::net::parse_ip_address(effective_advertised_udp_address);
        // Bind addresses and advertised UDP address may all be wildcard. A wildcard
        // advertised address is resolved by the client using the gRPC server IP.
    } catch (const std::exception& e) {
        log_error_fmt("ServerRuntime: invalid IP address configuration - {}", format_exception_message(e));
        stop_locked();
        return false;
    }

    const auto payload_bytes = static_cast<std::size_t>(effective_frame_count_) * frame_bytes;
    if (payload_bytes > aqua::config::UDP_AUDIO_PAYLOAD_BYTES) {
        log_error_fmt("ServerRuntime: AudioFrame payload {} exceeds UDP safe payload budget {}",
            payload_bytes, aqua::config::UDP_AUDIO_PAYLOAD_BYTES);
        stop_locked();
        return false;
    }

    try {
        reap_state_ = std::make_shared<ReapState>(ioc_);
    } catch (const std::bad_alloc&) {
        log_error("ServerRuntime: failed to allocate session reaper state");
        stop_locked();
        return false;
    }

    const auto advertised_udp_port = config_.advertised_udp_port.value_or(config_.udp_port);
    log_debug_fmt("ServerRuntime config: bind={} rpc_port={} udp_port={} advertised_udp={} format={}ch/{}Hz/enc={} packet_frames={} audio_queue_capacity={} session_timeout={}ms session_reap_interval={}ms capture_source={} device={}",
        config_.server_ip, config_.rpc_port, config_.udp_port,
        ::aqua::net::format_host_port(effective_advertised_udp_address, advertised_udp_port),
        effective_format_.channels, effective_format_.sample_rate, static_cast<int>(effective_format_.encoding),
        effective_frame_count_, config_.audio_queue_capacity_slots,
        config_.session_timeout.count(), config_.session_reap_interval.count(),
        static_cast<int>(config_.capture.source),
        effective_capture_device_ ? effective_capture_device_->value() : std::string("unresolved"));

    if (!device_mgr_) {
        log_error("ServerRuntime: audio device manager is unavailable on this platform");
        stop_locked();
        return false;
    }
    if (!effective_capture_device_) {
        log_error("ServerRuntime: capture device could not be resolved");
        stop_locked();
        return false;
    }
    capture_manager_ = std::make_unique<audio::CaptureManager>(*device_mgr_);
    log_debug("ServerRuntime: audio device manager and capture manager created");
    if (!capture_manager_->available()) {
        log_error("ServerRuntime: audio capture backend is unavailable on this platform");
        stop_locked();
        return false;
    }
    // 切换事务的生产者空档：丢弃属于旧设备的半个 pending 帧（保留 sequence，
    // 时间线不变式）。CaptureManager 不认识 packetizer，由 runtime 在此挂钩。
    capture_manager_->set_producer_gap_hook([this]() noexcept {
        packetizer_.discard_pending();
    });

    if (const auto bound = udp_.bind(config_.server_ip, config_.udp_port); !bound) {
        // 失败原因进日志：expected 化的意义就在这里（否则调用方只能看到一句
        // "bind 失败"，分不清端口占用 / 地址非法 / 已停止）。
        log_error_fmt("ServerRuntime: failed to bind UDP {}: {}",
            ::aqua::net::format_host_port(config_.server_ip, config_.udp_port),
            ::aqua::net::net_error_name(bound.error()));
        stop_locked();
        return false;
    }
    log_debug_fmt("ServerRuntime UDP ready: local_endpoint={}",
        ::aqua::net::format_host_port(config_.server_ip, udp_.local_endpoint().port()));
    if (const auto started = udp_.start(); !started) {
        log_error_fmt("ServerRuntime: failed to start UDP receive loop: {}",
            ::aqua::net::net_error_name(started.error()));
        stop_locked();
        return false;
    }
    log_debug_fmt("ServerRuntime UDP receive loop started on {}",
        ::aqua::net::format_host_port(config_.server_ip, udp_.local_endpoint().port()));
    if (!dispatcher_.start()) {
        log_error("ServerRuntime: failed to start audio network dispatcher");
        stop_locked();
        return false;
    }

    auto capture_cfg = config_.capture;
    if (config_.format) {
        capture_cfg.format = *config_.format;
    } else {
        capture_cfg.format.reset();
    }
    // 采集路由来自用户配置（capture_switching_design.md §4）：device 为空 =
    // FollowSystem，有值 = PreferredDevice。构造期解析的 effective_capture_device_
    // 只用于格式探测（packetizer 几何），不再钉给运行期流——设备故障/默认变化
    // 由 CaptureManager 候选链重建端点。首流若恰遇默认设备在探测与启动之间
    // 变化，下方格式校验会以明确错误拒绝（而非静默改变流几何）。
    const auto capture_start = capture_manager_->start(capture_cfg, [this](const audio::AudioBlock& block) noexcept { on_capture_block(block); }, [this](audio::AudioError error) noexcept { on_capture_event(error); });
    if (!capture_start) {
        log_error_fmt("ServerRuntime: failed to start audio capture: {}",
            audio::audio_error_name(capture_start.error()));
        stop_locked();
        return false;
    }
    if (capture_manager_->info().format != effective_format_) {
        log_error_fmt("ServerRuntime: capture backend selected format {}ch/{}Hz/enc={} but runtime expected {}ch/{}Hz/enc={}",
            capture_manager_->info().format.channels, capture_manager_->info().format.sample_rate,
            static_cast<int>(capture_manager_->info().format.encoding),
            effective_format_.channels, effective_format_.sample_rate,
            static_cast<int>(effective_format_.encoding));
        stop_locked();
        return false;
    }
    log_debug_fmt("ServerRuntime capture started: format={}ch/{}Hz frame_count={} source={}",
        capture_manager_->info().format.channels, capture_manager_->info().format.sample_rate,
        effective_frame_count_, static_cast<int>(capture_cfg.source));

    grpc_ = std::make_unique<grpc::GrpcServer>(
        *sessions_, effective_format_, effective_frame_count_,
        config_.server_ip, config_.rpc_port,
        grpc::AdvertisedUdpEndpoint { effective_advertised_udp_address, advertised_udp_port });
    if (!grpc_->is_started()) {
        log_error("ServerRuntime: gRPC server failed to start");
        stop_locked();
        return false;
    }
    log_debug("ServerRuntime gRPC server constructed and ready; starting worker thread");
    try {
        grpc_thread_ = std::jthread([this](std::stop_token st) {
            // stop 请求 → shutdown：run() 内部阻塞在 Wait()，不打断就永不返回
            // （jthread 析构的 auto-join 会挂死）。grpc_ 声明在线程之前，析构安全。
            std::stop_callback cb(st, [this] { grpc_->shutdown(); });
            grpc_->run();
        });
    } catch (const std::system_error& e) {
        log_error_fmt("ServerRuntime: failed to start gRPC worker thread: code={} message={}",
            e.code().value(), format_system_error_message(e.code()));
        stop_locked();
        return false;
    } catch (const std::exception& e) {
        log_error_fmt("ServerRuntime: failed to start gRPC worker thread: {}", format_exception_message(e));
        stop_locked();
        return false;
    } catch (...) {
        log_error("ServerRuntime: failed to start gRPC worker thread");
        stop_locked();
        return false;
    }

    RuntimeState expected = RuntimeState::Starting;
    (void)state_.compare_exchange_strong(expected, RuntimeState::Running,
        std::memory_order_acq_rel, std::memory_order_acquire);
    const auto reap = reap_state_;
    if (reap) {
        const auto weak_self = weak_from_this();
        try {
            asio::post(reap->strand, [reap, weak_self, interval = config_.session_reap_interval, timeout = config_.session_timeout] {
                schedule_reap(reap, weak_self, interval, timeout);
            });
        } catch (const std::exception& e) {
            log_error_fmt("ServerRuntime: failed to schedule session reaper: {}", format_exception_message(e));
            stop_locked();
            return false;
        } catch (...) {
            log_error("ServerRuntime: failed to schedule session reaper");
            stop_locked();
            return false;
        }
    }
    const auto final_state = state_.load(std::memory_order_acquire);
    log_debug_fmt("ServerRuntime startup completed with state={}", runtime_state_name(final_state));
    if (final_state == RuntimeState::Running || final_state == RuntimeState::Degraded) {
        log_info_fmt("ServerRuntime started: bind={} rpc={} udp={} advertise={} audio={}ch/{}Hz packet_frames={} audio_queue_capacity={} slots",
            config_.server_ip, config_.rpc_port, udp_.local_endpoint().port(),
            ::aqua::net::format_host_port(effective_advertised_udp_address, advertised_udp_port),
            effective_format_.channels, effective_format_.sample_rate,
            effective_frame_count_, config_.audio_queue_capacity_slots);
        return true;
    }
    return false;
}

void ServerRuntime::stop() noexcept
{
    std::lock_guard lock(lifecycle_mutex_);
    stop_locked();
}

aqua::diagnostics::ServerDiagnosticsSnapshot ServerRuntime::take_diagnostics_snapshot() const noexcept
{
    // capture_manager_ 的读取必须在 lifecycle_mutex_ 内：它与 switch_to/tick/stop
    // 并发时是 optional<string>/SwitchResult 的数据竞争。Client 侧快照已持锁，
    // Server 侧对称处理；临界区只做快照拷贝，成本可忽略。
    // 本方法不被任何已持该锁的方法调用，无嵌套死锁。
    std::lock_guard lock(lifecycle_mutex_);
    aqua::diagnostics::ServerDiagnosticsSnapshot snapshot;
    snapshot.state = state_.load(std::memory_order_acquire);
    snapshot.last_audio_error = last_audio_error_.load(std::memory_order_acquire);
    snapshot.capture_running = capture_running();
    snapshot.audio_format = effective_format_;
    snapshot.frame_count = effective_frame_count_;

    const auto cap = capture_stats();
    snapshot.capture.audio_events = cap.audio_events;
    snapshot.capture.packet_queries = cap.packet_queries;
    snapshot.capture.packet_empty = cap.packet_empty;
    snapshot.capture.packets_ready = cap.packets_ready;
    snapshot.capture.get_buffer_success = cap.get_buffer_success;
    snapshot.capture.callbacks = cap.callbacks;
    snapshot.capture.silent_callbacks = cap.silent_callbacks;
    snapshot.capture.synthetic_silence_blocks = cap.synthetic_silence_blocks;
    snapshot.capture.generated_silence_frames = cap.generated_silence_frames;
    snapshot.capture.starved_events = cap.starved_events;
    snapshot.capture.starved_ms = cap.starved_ms;
    snapshot.capture.state = cap.state;
    snapshot.capture.captured_frames = cap.captured_frames;
    snapshot.capture.captured_bytes = cap.captured_bytes;
    snapshot.capture.packet_frames_last = cap.packet_frames_last;
    snapshot.capture.packet_frames_min = cap.packet_frames_min;
    snapshot.capture.packet_frames_max = cap.packet_frames_max;
    snapshot.capture.current_starved_ms = cap.current_starved_ms;
    snapshot.capture.max_starved_ms = cap.max_starved_ms;

    if (capture_manager_) {
        auto& cs = snapshot.capture_switch;
        cs.state = capture_manager_->state();
        cs.route = capture_manager_->route_mode();
        cs.source = config_.capture.source;
        const auto active = capture_manager_->active_device();
        cs.active_device_id = active ? active->value() : std::string { };
        const auto requested = capture_manager_->preferred_device();
        cs.requested_device_id = requested ? requested->value() : std::string { };
        const auto switch_result = capture_manager_->last_switch_result();
        cs.last_outcome = switch_result ? switch_result->outcome : audio::SwitchOutcome::None;
        cs.last_switch_error = switch_result ? switch_result->last_error : audio::AudioError::None;
        cs.last_switch_duration_ms = switch_result ? switch_result->duration_ms : 0;
    }

    snapshot.packetizer.input_blocks = packetizer_.input_blocks();
    snapshot.packetizer.input_bytes = packetizer_.input_bytes();
    snapshot.packetizer.frames_emitted = packetizer_.frames_emitted();
    snapshot.packetizer.rejected_unaligned_blocks = packetizer_.rejected_unaligned_blocks();
    snapshot.packetizer.pending_discards = packetizer_.pending_discards();

    snapshot.queue.accepted_frames = frame_queue_.accepted_frames();
    snapshot.queue.consumed_frames = frame_queue_.consumed_frames();
    snapshot.queue.dropped_frames = frame_queue_.dropped_frames();
    snapshot.queue.depth_slots = frame_queue_.size_slots();
    snapshot.queue.high_watermark_slots = frame_queue_.high_watermark_slots();

    snapshot.dispatcher.frames_encoded = dispatcher_.frames_encoded();
    snapshot.dispatcher.frames_broadcast = dispatcher_.frames_broadcast();
    snapshot.dispatcher.frames_without_clients = dispatcher_.frames_without_clients();
    snapshot.dispatcher.encode_failures = dispatcher_.encode_failures();
    snapshot.dispatcher.dispatch_failures = dispatcher_.dispatch_failures();
    snapshot.dispatcher.dropped_frames = dispatcher_.dropped_frames();
    snapshot.dispatcher.published_frames = dispatcher_.published_frames();
    snapshot.dispatcher.worker_wakeups = dispatcher_.worker_wakeups();

    snapshot.net.transport = udp_.stats();
    snapshot.net.heartbeat_received = udp_.heartbeat_received();
    snapshot.net.heartbeat_rejected = udp_.heartbeat_rejected();
    snapshot.net.sessions_established = udp_.sessions_established();
    snapshot.net.sessions_refreshed = udp_.sessions_refreshed();
    snapshot.net.heartbeat_ack_attempts = udp_.heartbeat_ack_attempts();
    snapshot.net.malformed_datagrams = udp_.malformed_datagrams();
    snapshot.net.non_heartbeat_datagrams = udp_.non_heartbeat_datagrams();

    const auto sess = session_stats();
    snapshot.session.active = session_count();
    snapshot.session.created = sess.created;
    snapshot.session.connected = sess.connected;
    snapshot.session.refreshed = sess.refreshed;
    snapshot.session.removed = sess.removed;
    snapshot.session.expired = sess.expired;
    snapshot.session.removed_by_clear = sess.removed_by_clear;
    return snapshot;
}

void ServerRuntime::stop_locked() noexcept
{
    log_debug("ServerRuntime stop requested");
    if (!enter_stopping()) {
        return;
    }

    if (capture_manager_) {
        log_debug("ServerRuntime stopping capture manager");
        capture_manager_->stop();
    }
    if (reap_state_ != nullptr) {
        const auto reap = reap_state_;
        try {
            asio::post(reap->strand, [reap] {
                asio::error_code ec;
                reap->timer->cancel(ec);
            });
        } catch (...) {
            // 定时器由 State 共享并归 strand 所有；若 executor 已停止，
            // 最后一个 shared 状态的析构会取消未完成的等待。
        }
    }
    log_debug("ServerRuntime stopping audio network dispatcher");
    dispatcher_.stop();
    log_debug("ServerRuntime stopping UDP transport");
    udp_.stop();
    if (grpc_) {
        log_debug("ServerRuntime shutting down gRPC server");
        grpc_->shutdown();
    }
    // jthread：request_stop 触发线程体内 stop_callback（shutdown 幂等）；析构仍兜底。
    grpc_thread_.request_stop();
    if (grpc_thread_.joinable()) {
        grpc_thread_.join();
    }
    sessions_->clear();
    enter_stopped();
    log_info("ServerRuntime stopped");
}

void ServerRuntime::on_capture_block(const audio::AudioBlock& block) noexcept
{
    const auto state = state_.load(std::memory_order_acquire);
    if ((state != RuntimeState::Starting && state != RuntimeState::Running
            && state != RuntimeState::Degraded)
        || block.data.empty()) {
        return;
    }

    packetizer_.push(block.data, [this](const audio::AudioFrame& frame) noexcept {
        const auto result = frame_queue_.push(frame);
        if (result.accepted) {
            dispatcher_.publish_from_realtime(result.should_notify);
        }
    });
}

void ServerRuntime::on_capture_event(audio::AudioError error) noexcept
{
    if (error == audio::AudioError::None) {
        return;
    }
    last_audio_error_.store(error, std::memory_order_release);

    // 切换事务进行中（Switching）：该错误是事务 stop() 阶段投递的旧流滞留
    // 错误（stop 会 SetEvent(error_event) 并 join event 线程，旧流的临终
    // DeviceDisconnected 与 join 竞态，可能恰在事务窗口内回放）。路由由事务
    // 自身的候选链负责，不再置待处理标志——否则一次 tick/错误驱动的切换
    // 会在下一 control tick 叠加一次多余的 restart_on_error（二次 restart
    // 状态机缺陷，对称 client 侧 on_playback_event 的 Switching gate，
    // 见 playback_switching_design.md §14.4）。新流事务窗口内的真实错误
    // 由 silent_death 兜底（Running && !is_running -> 下一 tick 恢复）。
    if (capture_manager_ != nullptr
        && capture_manager_->state() == audio::CaptureSwitchState::Switching) {
        log_debug_fmt("server runtime: capture event {} during Switching, pending flag skipped (transaction owns routing)",
            audio::audio_error_name(error));
        return;
    }

    // 触发源白名单（capture_switching_design.md §6）：仅 DeviceDisconnected
    // 驱动切换——记录待处理标志，restart 由 service_capture_switching
    // （control tick，ioc 线程）执行；本回调运行在 backend event 线程，
    // stop/join/start 不得在此执行。设备错误不再迁移 Degraded（纯 capture
    // 设备错误不误杀会话）。禁止用 silence/low energy 等音频特征推断设备
    // 失效——"活着但无声"是 loopback 合法稳态。
    if (error == audio::AudioError::DeviceDisconnected) {
        capture_device_error_pending_.store(true, std::memory_order_release);
        log_warn_fmt("server runtime: capture device error {}, switch pending",
            audio::audio_error_name(error));
        return;
    }

    // 其余错误（后端内部错误等）：保持既有 Degraded 语义（网络/分发/后端
    // 致命原因，control timer 观察到后 stop）。
    auto state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == RuntimeState::Starting || state == RuntimeState::Running) {
            if (state_.compare_exchange_weak(state, RuntimeState::Degraded,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                log_warn_fmt("server runtime degraded: {}", audio::audio_error_name(error));
                return;
            }
            continue;
        }
        return;
    }
}

ServerRuntime::CaptureServiceAction ServerRuntime::service_capture_switching() noexcept
{
    // 快速路径：无待处理错误时仍需 tick（FollowSystem 默认跟随轮询），
    // 但 manager 未创建（未 start）时无事可做。
    if (capture_manager_ == nullptr) {
        return CaptureServiceAction::None;
    }

    std::lock_guard lock(lifecycle_mutex_);

    // 仅会话运行中服务：Stopping/Stopped/Degraded 交给既有终止路径；
    // Starting 由 start() 自身负责首次打开。
    if (state_.load(std::memory_order_acquire) != RuntimeState::Running) {
        return CaptureServiceAction::None;
    }

    const auto switch_state = capture_manager_->state();
    if (switch_state == audio::CaptureSwitchState::Fatal) {
        // Fatal 语义（§5）：链耗尽/预算超限 = server 无法提供所请求的
        // 音频源，会话终止（无 capture 的会话无意义）。
        return CaptureServiceAction::Fatal;
    }

    const bool had_error = capture_device_error_pending_.exchange(false, std::memory_order_acq_rel);
    // 静默死流兜底（对称 client 侧 service_playback_recovery）：管理状态认为
    // Running 但 backend 已停止（如音频线程退出而 event 线程已结束，错误无处
    // 投递）。这种情形下没有错误事件、诊断却显示"运行中"，server 会永久静默，
    // 因此与设备错误同等对待，走同一 restart 事务（同样受重试预算约束）。
    const bool silent_death = !had_error
        && capture_manager_->state() == audio::CaptureSwitchState::Running
        && !capture_manager_->is_running();

    if (had_error || silent_death) {
        log_info_fmt("server runtime: starting capture recovery (trigger={})",
            had_error ? "device_error" : "silent_stream_death");
        const auto result = capture_manager_->restart_on_error();
        const auto after = capture_manager_->state();
        if (after == audio::CaptureSwitchState::Fatal) {
            return CaptureServiceAction::Fatal;
        }
        if (result.has_value() && after == audio::CaptureSwitchState::Running) {
            // 事务成功 = 设备错误已被此次切换处理完毕：清零锁存错误。
            // 时序安全：旧流临终错误在事务 stop() 阶段 latch（join 保证
            // 先于事务返回），清零必在其后（对称 client 侧修复，见
            // playback_switching_design.md §14.4）。
            last_audio_error_.store(audio::AudioError::None, std::memory_order_release);
            return CaptureServiceAction::Restarted;
        }
        return CaptureServiceAction::None;
    }

    // 无待处理错误：路由轮询（FollowSystem 跟随系统默认设备变化；
    // 决策与查询都在 CaptureManager 内）。tick 内部预算超限也会落 Fatal。
    const bool followed = capture_manager_->tick();
    const auto after_follow = capture_manager_->state();
    if (after_follow == audio::CaptureSwitchState::Fatal) {
        return CaptureServiceAction::Fatal;
    }
    if (followed && after_follow == audio::CaptureSwitchState::Running) {
        // tick 驱动的事务成功 = 同一次设备变化已被处理完毕：吸收事务
        // 前后 latch 的待处理设备错误标志，否则旧流临终错误（在事务
        // stop() 阶段、gate 尚不可见的窗口内置位）会让下一 control tick
        // 对同一事件做第二次 restart（对称 client 侧 service_devices_changed
        // 的吸收逻辑）。时序安全：旧流临终错误在事务 stop() 阶段 latch
        // （join 保证先于事务返回），清零必在其后；新流事务窗口内的真实
        // 错误被吸收的残余风险由 silent_death 兜底。
        capture_device_error_pending_.store(false, std::memory_order_release);
        last_audio_error_.store(audio::AudioError::None, std::memory_order_release);
        return CaptureServiceAction::Restarted;
    }
    return CaptureServiceAction::None;
}

void ServerRuntime::schedule_reap(const std::shared_ptr<ReapState>& reap,
    const std::weak_ptr<ServerRuntime>& weak_self,
    std::chrono::milliseconds interval, std::chrono::milliseconds timeout)
{
    if (!reap || !reap->timer) {
        return;
    }
    reap->timer->expires_after(interval);
    reap->timer->async_wait(asio::bind_executor(reap->strand,
        [reap, weak_self, interval, timeout](const asio::error_code& ec) {
            if (ec) {
                return;
            }
            const auto self = weak_self.lock();
            if (!self) {
                return;
            }
            const auto state = self->state_.load(std::memory_order_acquire);
            if (state != RuntimeState::Running && state != RuntimeState::Degraded) {
                return;
            }
            self->sessions_->remove_expired_sessions(timeout);
            schedule_reap(reap, weak_self, interval, timeout);
        }));
}

} // namespace aqua::runtime
