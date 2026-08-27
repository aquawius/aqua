#include "aqua/net/udp/udp_transport.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"

#include <utility>

namespace aqua::net {

// 构造：创建独立的 State（strand + 绑定到 strand 的 socket）。
// 此时不打开 socket；打开由 bind() / open() / set_remote() 触发。
UdpTransport::UdpTransport(asio::io_context& ioc)
    : state_(std::make_shared<State>(ioc))
{
}

// 析构：自动 stop() 关闭 socket、清空发送队列。
// State 由 shared_ptr 管理：即使已排队的异步 handler 尚未执行，State 也存活，
// 不会访问到已析构的 transport（详见头文件生命周期模型说明）。
UdpTransport::~UdpTransport()
{
    stop();
}

bool UdpTransport::bind(const std::string& bind_ip, std::uint16_t port)
{
    return open_and_bind(bind_ip, port, /*reuse_address=*/true);
}

bool UdpTransport::open()
{
    if (is_open()) {
        return true; // 幂等：已打开直接成功
    }
    // 客户端不需要 SO_REUSEADDR：绑定的是临时端口，不存在固定端口复用冲突。
    return open_and_bind("0.0.0.0", 0, /*reuse_address=*/false);
}

bool UdpTransport::open_and_bind(const std::string& bind_ip, std::uint16_t port, bool reuse_address)
{
    const auto& state = state_;
    if (state->stopped.load(std::memory_order_acquire)) {
        log_debug("UdpTransport open ignored: transport already stopped");
        return false;
    }

    try {
        const auto bind_address = ::aqua::net::parse_ip_address(bind_ip);

        // 重复 open/bind 必须是同一 endpoint 才视为幂等成功；如果调用方试图改绑定
        // 地址或端口，不能静默成功，否则上层会以为新 endpoint 已生效。切换地址族
        // 或绑定位置请创建新的 transport。
        // 注意 port=0 的边界：首次以 port=0 绑定会由 OS 分配临时端口，local_endpoint
        // 记录的是实际端口；此时若再次以 port=0 调用，会因 0 != 实际端口被当作
        // "不同 endpoint"而拒绝。需要幂等重入请传实际端口（open() 路径有
        // is_open() 短路，不会踩到该分支）。
        if (is_open()) {
            const auto& current = state->local_endpoint;
            if (bind_address == current.address() && port == current.port()) {
                log_debug_fmt("UdpTransport open ignored: transport already bound on {}",
                    current.address().to_string());
                return true;
            }
            log_debug_fmt("UdpTransport open rejected: already bound on {}:{}, requested {}:{}",
                current.address().to_string(), current.port(), bind_ip, port);
            return false;
        }
        const auto protocol = bind_address.is_v6() ? asio::ip::udp::v6() : asio::ip::udp::v4();

        // 根据 bind 地址族打开对应 UDP socket。本函数在调用线程执行同步 socket 操作是安全的：
        // 打开成功前不会有任何在途异步操作与这些调用竞争。
        state->socket.open(protocol);
        if (bind_address.is_v6()) {
            // 明确使用 IPv6-only，避免同一个 listener 出现原生 IPv6 与
            // IPv4-mapped IPv6 两种 endpoint 表示。双栈请分别创建 IPv4/IPv6 实例。
            state->socket.set_option(asio::ip::v6_only(true));
        }

        // SO_REUSEADDR 仅在 POSIX 上启用，且必须在 bind 之前设置（bind 之后再设
        // 对本次绑定无效）。Windows 语义不同：它允许两个进程静默绑定同一 UDP
        // 端口，datagram 会被随机分流到其中一个进程，表现为"server 启动正常但
        // 收不到 HELLO"——排障极其困难。Client 绑定 port 0、server 正常关闭都
        // 不产生需要复用的 TIME_WAIT 状态，因此 Windows 上直接不设置。
#ifndef _WIN32
        if (reuse_address) {
            state->socket.set_option(asio::ip::udp::socket::reuse_address(true));
        }
#else
        (void)reuse_address; // 避免 Windows 构建产生 unused parameter 告警
#endif

        // 显式设置内核接收缓冲区：Windows 默认约 8KB，高负载下易丢包。
        // 用 error_code 版本避免失败中断，仅记录 debug 便于排查。
        asio::error_code rcvbuf_ec;
        state->socket.set_option(
            asio::socket_base::receive_buffer_size(config::UDP_RECV_BUFFER_BYTES), rcvbuf_ec);
        if (rcvbuf_ec) {
            log_debug_fmt("UdpTransport set SO_RCVBUF failed on {}:{} - {}",
                bind_ip, port, rcvbuf_ec.message());
        }

        // 显式设置内核发送缓冲区：用于吸收短时间发送突发（应用层仍有独立的
        // 有界 datagram 队列，见 UDP_MAX_QUEUED_DATAGRAMS）。
        asio::error_code sndbuf_ec;
        state->socket.set_option(
            asio::socket_base::send_buffer_size(config::UDP_SEND_BUFFER_BYTES), sndbuf_ec);
        if (sndbuf_ec) {
            log_debug_fmt("UdpTransport set SO_SNDBUF failed on {}:{} - {}",
                bind_ip, port, sndbuf_ec.message());
        }

        // 绑定本地地址；bind_ip 为 "0.0.0.0" 表示监听所有接口。
        const auto ep = asio::ip::udp::endpoint(bind_address, port);
        state->socket.bind(ep);
        // 保存本地 endpoint 快照：之后 local_endpoint() 直接返回快照，
        // 避免跨线程访问 socket（bind 后 local_endpoint 不再变化）。
        state->local_endpoint = state->socket.local_endpoint();
        state->open.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception& e) {
        // 绑定失败（端口被占用、地址非法等）：关闭 socket、复位状态并上报。
        asio::error_code ec;
        state->socket.close(ec);
        state->local_endpoint = { };
        state->open.store(false, std::memory_order_release);
        log_error_fmt("UdpTransport bind failed on {}:{} - {}", bind_ip, port, e.what());
        return false;
    }
}

// 设置默认发送目标（endpoint 版）。先校验端口非 0，再确保 socket 已打开。
bool UdpTransport::set_remote(const asio::ip::udp::endpoint& remote)
{
    // 端口 0 不是合法对端（0 表示"未指定/通配"），直接拒绝，避免后续
    // send 把数据发往无效目标。
    if (remote.port() == 0) {
        log_error("UdpTransport::set_remote rejected: remote port is 0");
        return false;
    }
    if (!is_open()) {
        // 未打开时根据远端地址族选择 socket：IPv4 绑定 0.0.0.0:0，
        // IPv6 绑定 :::0。打开 socket 后地址族不可再切换，因此必须在
        // 第一次 set_remote() 时决定。
        const char* bind_ip = remote.address().is_v6() ? "::" : "0.0.0.0";
        if (!open_and_bind(bind_ip, 0, /*reuse_address=*/false)) {
            return false;
        }
    } else if (local_endpoint().address().is_v4() != remote.address().is_v4()) {
        log_error("UdpTransport::set_remote rejected: remote address family differs from open socket");
        return false;
    }
    {
        // 加锁写入：send() 线程可能正在 remote_endpoint() 读它。
        std::lock_guard lock(remote_mutex_);
        remote_ = remote;
    }
    return true;
}

// 设置默认发送目标（字符串版）：解析 IP 字面量（不支持 DNS 主机名）。IPv6
// 地址可带方括号，例如 [2001:db8::1]；本函数会自动选择 IPv6 socket。
bool UdpTransport::set_remote(const std::string& server_ip, std::uint16_t port)
{
    if (port == 0) {
        log_error_fmt("UdpTransport::set_remote rejected: remote port is 0 for {}", server_ip);
        return false;
    }
    try {
        return set_remote(asio::ip::udp::endpoint(::aqua::net::parse_ip_address(server_ip), port));
    } catch (const std::exception& e) {
        // make_address 对非法 IP 字面量抛异常，转为返回 false 并记录。
        log_error_fmt("UdpTransport set_remote failed: invalid address {}:{} - {}",
            server_ip, port, e.what());
        return false;
    }
}

// 是否已设置默认发送目标：端口 0 表示未设置。
bool UdpTransport::has_remote() const noexcept
{
    std::lock_guard lock(remote_mutex_);
    return remote_.port() != 0;
}

// 获取默认发送目标（锁内拷贝，返回快照，不暴露内部引用）。
asio::ip::udp::endpoint UdpTransport::remote_endpoint() const noexcept
{
    std::lock_guard lock(remote_mutex_);
    return remote_;
}

// 启动接收循环。先做快速检查（未打开/已停止直接拒绝），真正的状态切换
// （handler 赋值、receiving 标志、首次投递）dispatch 到 strand 上执行，
// 这样即使多个调用线程竞争启动接收也是安全的。
bool UdpTransport::start_receive(ReceiveHandler handler)
{
    const auto& state = state_;
    if (!state->open.load(std::memory_order_acquire)
        || state->stopped.load(std::memory_order_acquire)) {
        log_debug("UdpTransport::start_receive ignored: socket is not open");
        return false;
    }

    // start_receive() is normally called before io_context::run(). Serialising the state
    // transition on the transport strand keeps it safe even when multiple caller threads
    // race to start reception.
    asio::dispatch(state->strand, [state, handler = std::move(handler)]() mutable {
        // 到达 strand 时可能已被 stop()/close（dispatch 排在关闭任务之后），再查一次。
        if (state->stopped.load(std::memory_order_acquire)
            || !state->socket.is_open()) {
            return;
        }
        // 防重复启动：receiving 为 true 说明已有接收循环在跑，忽略第二次调用。
        if (state->receiving) {
            log_warn("UdpTransport::start_receive called twice, ignoring");
            return;
        }
        state->handler = std::move(handler);
        state->receiving = true;
        do_receive(state); // 投递第一个 async_receive_from
    });
    return true;
}

// 定向发送（拷贝语义）：把 data 复制进新分配的 shared_ptr 缓冲后走 send_to_shared 入队。
// 异常（bad_alloc 等）不允许传出——调用方可能是 packetizer 或 IO 回调线程，
// 抛出会使其异常退出；统一降级为 debug 日志。
void UdpTransport::send_to(const asio::ip::udp::endpoint& target,
    std::span<const std::byte> data)
{
    try {
        auto buf = std::make_shared<std::vector<std::byte>>(data.begin(), data.end());
        send_to_shared(target, std::move(buf));
    } catch (const std::exception& e) {
        log_debug_fmt("UDP send not queued: {}", e.what());
    } catch (...) {
        log_debug("UDP send not queued: unknown exception");
    }
}

// 定向发送（共享缓冲）：调用线程只进入一个真正有界的 MPSC 风格用户态队列。
// 队列由 mutex 保护，strand 只负责 socket async_send_to；连续多个 send_to_shared()
// 最多触发一个等待中的发送泵 task，从结构上避免"一个 datagram 一个 asio::post"
// 导致 executor 队列本身无界增长。
void UdpTransport::send_to_shared(
    const asio::ip::udp::endpoint& target,
    std::shared_ptr<const std::vector<std::byte>> data)
{
    if (!data || data->empty() || target.port() == 0) {
        return;
    }

    bool need_schedule = false;
    try {
        {
            std::lock_guard lock(state_->tx_queue_mutex);
            if (state_->stopped.load(std::memory_order_acquire)) {
                return;
            }

            // Only pending items are droppable. The in-flight item is owned separately
            // and can never be removed by queue overflow.
            if (state_->send_queue.size() >= config::UDP_MAX_QUEUED_DATAGRAMS) {
                state_->send_queue.pop_front();
                state_->tx_dropped.fetch_add(1, std::memory_order_relaxed);
            }

            state_->send_queue.push_back(PendingSend { target, std::move(data) });
            state_->tx_queue_depth.store(state_->send_queue.size(), std::memory_order_release);
            if (!state_->send_pump_scheduled) {
                state_->send_pump_scheduled = true;
                need_schedule = true;
            }
        }

        if (need_schedule) {
            asio::post(state_->strand, [state = state_] { start_next_send(state); });
        }
    } catch (const std::exception& e) {
        if (need_schedule) {
            std::lock_guard lock(state_->tx_queue_mutex);
            state_->send_pump_scheduled = false;
        }
        log_debug_fmt("UDP send not queued: {}", e.what());
    } catch (...) {
        if (need_schedule) {
            std::lock_guard lock(state_->tx_queue_mutex);
            state_->send_pump_scheduled = false;
        }
        log_debug("UDP send not queued: unknown exception");
    }
}

// 便捷发送：先取默认目标快照（锁内拷贝，避免持锁调用定向发送），再走拷贝语义入队。
void UdpTransport::send(std::span<const std::byte> data)
{
    const auto remote = remote_endpoint();
    if (remote.port() == 0) {
        log_debug("UdpTransport::send ignored: remote endpoint not set");
        return;
    }
    send_to(remote, data);
}

// 便捷发送（共享缓冲版）：同上，走 send_to_shared 的零拷贝队列路径。
void UdpTransport::send_shared(std::shared_ptr<const std::vector<std::byte>> data)
{
    const auto remote = remote_endpoint();
    if (remote.port() == 0) {
        log_debug("UdpTransport::send_shared ignored: remote endpoint not set");
        return;
    }
    send_to_shared(remote, std::move(data));
}

// 发送泵（strand 上执行）：从唯一的有界队列中串行发送 datagram。
// 队列本身由 tx_queue_mutex 保护，因为 send_to_shared() 可以由任意业务/音频线程调用。
// pump 在存在在途 async_send_to 时保持 scheduled 状态；发送完成后继续泵送下一项。
// 队列清空时才释放 scheduled 状态，这样下一个生产者会重新安排一个 task。
void UdpTransport::start_next_send(const std::shared_ptr<State>& state)
{
    if (state->stopped.load(std::memory_order_acquire) || !state->socket.is_open()) {
        std::lock_guard lock(state->tx_queue_mutex);
        // Keep in_flight alive until the async_send_to completion handler runs. The
        // completion handler itself owns State, so the buffer remains valid even when
        // stop() is called concurrently with an in-flight send.
        state->send_pump_scheduled = false;
        state->send_queue.clear();
        state->tx_queue_depth.store(0, std::memory_order_release);
        return;
    }

    {
        std::lock_guard lock(state->tx_queue_mutex);
        if (state->in_flight.has_value()) {
            return;
        }
        if (state->send_queue.empty()) {
            state->send_pump_scheduled = false;
            state->tx_queue_depth.store(0, std::memory_order_release);
            return;
        }
        state->in_flight.emplace(std::move(state->send_queue.front()));
        state->send_queue.pop_front();
        state->tx_queue_depth.store(state->send_queue.size(), std::memory_order_release);
    }

    auto payload = state->in_flight->payload;
    auto target = state->in_flight->target;

    state->socket.async_send_to(
        asio::buffer(*payload), target,
        asio::bind_executor(state->strand,
            [state](const asio::error_code& ec, std::size_t sent) {
                {
                    std::lock_guard lock(state->tx_queue_mutex);
                    state->in_flight.reset();
                    state->tx_queue_depth.store(state->send_queue.size(), std::memory_order_release);
                    if (state->send_queue.empty()
                        || state->stopped.load(std::memory_order_acquire)) {
                        state->send_pump_scheduled = false;
                    }
                }

                if (ec) {
                    state->tx_errors.fetch_add(1, std::memory_order_relaxed);
                    if (ec != asio::error::operation_aborted
                        && !state->stopped.load(std::memory_order_acquire)) {
                        log_debug_fmt("UDP send failed: {}", ec.message());
                    }
                } else {
                    state->tx_packets.fetch_add(1, std::memory_order_relaxed);
                    state->tx_bytes.fetch_add(sent, std::memory_order_relaxed);
                }

                if (!state->stopped.load(std::memory_order_acquire)) {
                    start_next_send(state);
                }
            }));
}

// 停止：幂等。置 stopped 后 post 关闭任务到 strand；关闭任务只持有 State，
// 不捕获 this，因此即使 transport 析构早于关闭任务执行也不会 UAF。
void UdpTransport::stop() noexcept
{
    const auto state = state_;
    if (state->stopped.exchange(true, std::memory_order_acq_rel)) {
        return; // 已经停止，跳过重复关闭
    }
    state->open.store(false, std::memory_order_release);

    try {
        // close 必须与 async_send_to / async_receive_from 在同一 strand 串行
        // 执行，这里只 post，不在调用方线程直接碰 socket。
        asio::post(state->strand, [state] { close_state(state); });
    } catch (const std::exception& e) {
        // post 失败（极少见，如 executor 不再可用）：不跨线程直接操作 socket，
        // 避免破坏 strand 的并发边界；最终由 State 析构关闭底层句柄。此时调用方
        // 应保持 io_context 运行直到关闭任务可执行，或在 stop() 后尽快析构 transport。
        log_debug_fmt("UdpTransport stop could not be queued: {}", e.what());
    } catch (...) {
        log_debug("UdpTransport stop could not be queued: unknown exception");
        // 同上：若 io_context 已无法继续执行 handler，最终由 State 析构释放句柄。
    }
}

// 关闭 State：复位接收/发送标志、清空队列与回调，然后 cancel + close socket。
// cancel 使在途 async_receive_from / async_send_to 以 operation_aborted 完成，
// 其回调随后复位自己的标志并停止续发。noexcept：析构/回滚路径不能抛异常。
void UdpTransport::close_state(const std::shared_ptr<State>& state) noexcept
{
    state->receiving = false;
    {
        std::lock_guard lock(state->tx_queue_mutex);
        state->send_pump_scheduled = false;
        state->send_queue.clear();
        state->tx_queue_depth.store(0, std::memory_order_release);
    }
    state->handler = { };

    asio::error_code ec;
    state->socket.cancel(ec); // 取消在途异步操作
    state->socket.close(ec); // 关闭底层句柄
}

// 是否已打开：读 open 快照（bind/open 成功时由 open_and_bind 写入）。
bool UdpTransport::is_open() const noexcept
{
    return state_->open.load(std::memory_order_acquire);
}

// 返回 bind 成功时的本地 endpoint 快照（open_and_bind 中保存），不访问 socket，
// 因此线程安全且不会抛异常。
asio::ip::udp::endpoint UdpTransport::local_endpoint() const noexcept
{
    return state_->local_endpoint;
}

// 聚合统计快照。计数项均为 relaxed 读，多线程下是近似值；tx_queue_depth 用
// acquire 读，表示最近一次队列深度快照。
UdpTransportStats UdpTransport::stats() const noexcept
{
    const auto& state = state_;
    return UdpTransportStats {
        state->rx_packets.load(std::memory_order_relaxed),
        state->rx_bytes.load(std::memory_order_relaxed),
        state->rx_errors.load(std::memory_order_relaxed),
        state->tx_packets.load(std::memory_order_relaxed),
        state->tx_bytes.load(std::memory_order_relaxed),
        state->tx_errors.load(std::memory_order_relaxed),
        state->tx_dropped.load(std::memory_order_relaxed),
        state->tx_queue_depth.load(std::memory_order_acquire),
    };
}

// 接收循环：投递下一个 async_receive_from 并处理收包。仅在 strand 上调用。
void UdpTransport::do_receive(const std::shared_ptr<State>& state)
{
    // 已停止或 socket 未打开：退出接收循环并复位标志，不再投递。
    if (state->stopped.load(std::memory_order_acquire) || !state->socket.is_open()) {
        state->receiving = false;
        return;
    }

    state->socket.async_receive_from(
        asio::buffer(state->recv_buf), state->recv_endpoint,
        asio::bind_executor(state->strand,
            [state](const asio::error_code& ec, std::size_t bytes) {
                if (ec) {
                    // operation_aborted：socket 被 stop() 取消/关闭，正常退出，
                    // 不再投递接收。
                    if (ec == asio::error::operation_aborted
                        || state->stopped.load(std::memory_order_acquire)) {
                        state->receiving = false;
                        return;
                    }
                    // 其它错误（如对端关闭后内核回送的 ICMP port unreachable /
                    // connection_refused / connection_reset）不应终止接收循环：
                    // server 仍需为其它 session 接收数据。降为 debug 避免日志风暴。
                    state->rx_errors.fetch_add(1, std::memory_order_relaxed);
                    log_debug_fmt("UDP recv error: {}", ec.message());
                    if (state->socket.is_open()) {
                        do_receive(state); // 继续保活接收循环
                    } else {
                        state->receiving = false;
                    }
                    return;
                }

                // 成功收包：更新统计后上交回调。
                state->rx_packets.fetch_add(1, std::memory_order_relaxed);
                state->rx_bytes.fetch_add(bytes, std::memory_order_relaxed);

                if (state->handler) {
                    try {
                        state->handler(state->recv_endpoint,
                            std::span<const std::byte> { state->recv_buf.data(), bytes });
                    } catch (const std::exception& e) {
                        // 用户回调不能把异常带出 asio handler，否则 IO 线程会
                        // 直接 terminate，表现为"网络突然静默"。继续保活接收循环。
                        log_error_fmt("UDP receive handler exception: {}", e.what());
                    } catch (...) {
                        log_error("UDP receive handler unknown exception");
                    }
                }

                // 回调执行期间 stop() 可能已发生，此时不应再投递新的接收。
                if (!state->stopped.load(std::memory_order_acquire)) {
                    do_receive(state); // 继续接收
                } else {
                    state->receiving = false;
                }
            }));
}

} // namespace aqua::net
