#include "core/net/transport/udp_socket_base.h"

#include "core/logger/logger.h"

#include <utility>

namespace aqua::net {

// 构造：创建独立的 State（strand + 绑定到 strand 的 socket）。
// 此时不打开 socket；打开/绑定由派生类（UdpServer::bind / UdpClient::open）触发。
UdpSocketBase::UdpSocketBase(asio::io_context& ioc)
    : state_(std::make_shared<State>(ioc))
{
}

// 析构：自动 stop() 关闭 socket、清空发送队列。
// State 由 shared_ptr 管理：即使已排队的异步 handler 尚未执行，State 也存活，
// 不会访问到已析构的 transport（详见头文件生命周期模型说明）。
UdpSocketBase::~UdpSocketBase()
{
    stop();
}

bool UdpSocketBase::open_and_bind(const std::string& bind_ip, std::uint16_t port, bool reuse_address)
{
    // 已停止的 transport 不可复用：此时再打开只会得到一个"看似打开实则停摆"的
    // socket（send / 接收循环都会因 stopped 静默退出），直接拒绝。
    const auto& state = state_;
    if (state->stopped.load(std::memory_order_acquire)) {
        log_debug("UdpSocket open_and_bind ignored: transport already stopped");
        return false;
    }

    try {
        // 打开 IPv4 UDP socket。本函数在调用线程执行同步 socket 操作是安全的：
        // 打开成功前不会有任何在途异步操作与这些调用竞争。
        state->socket.open(asio::ip::udp::v4());

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
            log_debug_fmt("UdpSocket set SO_RCVBUF failed on {}:{} - {}",
                bind_ip, port, rcvbuf_ec.message());
        }

        // 显式设置内核发送缓冲区：用于吸收短时间发送突发（应用层仍有独立的
        // 有界 datagram 队列，见 UDP_MAX_QUEUED_DATAGRAMS）。
        asio::error_code sndbuf_ec;
        state->socket.set_option(
            asio::socket_base::send_buffer_size(config::UDP_SEND_BUFFER_BYTES), sndbuf_ec);
        if (sndbuf_ec) {
            log_debug_fmt("UdpSocket set SO_SNDBUF failed on {}:{} - {}",
                bind_ip, port, sndbuf_ec.message());
        }

        // 绑定本地地址；bind_ip 为 "0.0.0.0" 表示监听所有接口。
        const auto ep = asio::ip::udp::endpoint(asio::ip::make_address(bind_ip), port);
        state->socket.bind(ep);
        // 保存本地 endpoint 快照：之后 socket_local_endpoint() 直接返回快照，
        // 避免跨线程访问 socket（bind 后 local_endpoint 不再变化）。
        state->local_endpoint = state->socket.local_endpoint();
        state->open.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception& e) {
        // 绑定失败（端口被占用、地址非法等）：关闭 socket、复位状态并上报。
        asio::error_code ec;
        state->socket.close(ec);
        state->local_endpoint = {};
        state->open.store(false, std::memory_order_release);
        log_error_fmt("UdpSocket bind failed on {}:{} - {}", bind_ip, port, e.what());
        return false;
    }
}

// 启动接收循环。先做快速检查（未打开/已停止直接拒绝），真正的状态切换
// （handler 赋值、receiving 标志、首次投递）dispatch 到 strand 上执行，
// 这样即使多个调用线程竞争启动接收也是安全的。
bool UdpSocketBase::start_receive(ReceiveHandler handler)
{
    const auto& state = state_;
    if (!state->open.load(std::memory_order_acquire)
        || state->stopped.load(std::memory_order_acquire)) {
        log_debug("UdpSocket::start_receive ignored: socket is not open");
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
            log_warn("UdpSocket::start_receive called twice, ignoring");
            return;
        }
        state->handler = std::move(handler);
        state->receiving = true;
        do_receive(state); // 投递第一个 async_receive_from
    });
    return true;
}

// 拷贝语义发送：把 data 复制进新分配的 shared_ptr 缓冲后走 send_shared 入队。
// 异常（bad_alloc 等）不允许传出——调用方可能是 packetizer 或 IO 回调线程，
// 抛出会使其异常退出；统一降级为 debug 日志。
void UdpSocketBase::send_copy(const asio::ip::udp::endpoint& target,
    std::span<const std::byte> data)
{
    try {
        auto buf = std::make_shared<std::vector<std::byte>>(data.begin(), data.end());
        send_shared(target, std::move(buf));
    } catch (const std::exception& e) {
        log_debug_fmt("UDP send not queued: {}", e.what());
    } catch (...) {
        log_debug("UDP send not queued: unknown exception");
    }
}

// 共享缓冲发送：调用线程只做快速检查 + post 入队，实际入队与发送在 strand 上完成。
// 队列有界（UDP_MAX_QUEUED_DATAGRAMS）：超限时丢弃最旧 datagram——实时音频
// 场景下旧的音频包比新的更没有价值，drop-oldest 能同时压低延迟与内存占用。
void UdpSocketBase::send_shared(const asio::ip::udp::endpoint& target,
    std::shared_ptr<const std::vector<std::byte>> data)
{
    const auto state = state_;
    if (!data || state->stopped.load(std::memory_order_acquire)) {
        return; // 空缓冲或已停止：不投递，避免在关闭流程中触碰 socket
    }

    try {
        // post 到 strand：发送与接收/关闭串行化，避免不同线程并发发起
        // async_send_to / async_receive_from（asio socket 非线程安全）。
        asio::post(state->strand, [state, target, buf = std::move(data)]() mutable {
            // 到达 strand 时可能已被 stop()/close，再查一次。
            if (state->stopped.load(std::memory_order_acquire) || !state->socket.is_open()) {
                return;
            }

            // 队列超限：丢弃最旧的 datagram（实时音频宁可丢旧保新）。
            // 注意：此策略只做"内存有界"，不做"延迟有界"——若上层持续快于发送
            // 速率，队列会维持在接近上限的深度，落后期间的旧音频仍会被泵依次
            // 发送出去（新鲜度权衡见 start_next_send 的注释）。
            if (state->send_queue.size() >= config::UDP_MAX_QUEUED_DATAGRAMS) {
                state->send_queue.pop_front();
                state->tx_dropped.fetch_add(1, std::memory_order_relaxed);
            }

            state->send_queue.push_back(PendingSend { target, std::move(buf) });
            state->tx_queue_depth.store(state->send_queue.size(), std::memory_order_release);
            start_next_send(state); // 若当前空闲则立即开始发送
        });
    } catch (const std::exception& e) {
        log_debug_fmt("UDP send not queued: {}", e.what());
    } catch (...) {
        log_debug("UDP send not queued: unknown exception");
    }
}

// 发送泵（strand 上执行）：串行发送队列中的 datagram。
// 只有"无在途发送 && 队列非空 && 未停止 && socket 打开"时才发起下一个发送，
// 保证同一时刻至多一个 async_send_to 在途。泵总是从队首（最老）开始发送，
// 发送完成回调再次调用自身以续发下一个。
//
// 设计权衡（实时音频新鲜度）：队列只做"内存有界"（UDP_MAX_QUEUED_DATAGRAMS），
// 不做"延迟有界"。若上层因 scheduler stall 等原因落后（例如已积累 200ms 音频），
// 本泵仍会努力把旧音频依次发出，与实时音频"宁可丢旧保新"的哲学存在张力。
// 将来若需要延迟有界：可给 PendingSend 打入队时间戳（steady_clock），在泵取出
// 队首时丢弃年龄超过阈值（如 UDP_MAX_DATAGRAM_AGE_MS 配置项）的项，既保新鲜度
// 又不牺牲吞吐。
void UdpSocketBase::start_next_send(const std::shared_ptr<State>& state)
{
    if (state->send_in_flight || state->send_queue.empty()
        || state->stopped.load(std::memory_order_acquire) || !state->socket.is_open()) {
        return;
    }

    // 取队首。target/payload 先拷贝成局部变量：完成回调里会 pop_front 队首，
    // async_send_to 期间不能依赖队列项；局部 shared_ptr 同时保证 payload 保活。
    state->send_in_flight = true;
    const auto& item = state->send_queue.front();
    const auto target = item.target;
    const auto payload = item.payload;

    state->socket.async_send_to(
        asio::buffer(*payload), target,
        // 完成回调绑定到 strand，保证与接收循环/队列操作串行。
        asio::bind_executor(state->strand,
            [state, payload](const asio::error_code& ec, std::size_t sent) {
                // 先复位在途标志、弹出队首并更新队列深度，再决定是否续发。
                state->send_in_flight = false;
                if (!state->send_queue.empty()) {
                    state->send_queue.pop_front();
                }
                state->tx_queue_depth.store(state->send_queue.size(), std::memory_order_release);

                if (ec) {
                    state->tx_errors.fetch_add(1, std::memory_order_relaxed);
                    // operation_aborted（stop 取消）属预期退出，不再刷日志；
                    // 其它失败（如对端关闭触发 ICMP connection_refused）也是
                    // 预期网络事件，降为 debug。session 死亡判据应由 recv 超时
                    // 驱动，而非 send 失败。
                    if (ec != asio::error::operation_aborted
                        && !state->stopped.load(std::memory_order_acquire)) {
                        log_debug_fmt("UDP send failed: {}", ec.message());
                    }
                } else {
                    state->tx_packets.fetch_add(1, std::memory_order_relaxed);
                    state->tx_bytes.fetch_add(sent, std::memory_order_relaxed);
                }
                start_next_send(state); // 续发下一个待发送 datagram
            }));
}

// 停止：幂等。置 stopped 后 post 关闭任务到 strand；关闭任务只持有 State，
// 不捕获 this，因此即使 transport 析构早于关闭任务执行也不会 UAF。
void UdpSocketBase::stop() noexcept
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
        // post 失败（如 io_context 已停止、handler 不再会执行）：退化为直接
        // 同步关闭。此时不会有新异步操作被调度，同步关闭是安全的。
        log_debug_fmt("UdpSocket stop could not be queued: {}", e.what());
        close_state(state);
    } catch (...) {
        log_debug("UdpSocket stop could not be queued: unknown exception");
        close_state(state);
    }
}

// 关闭 State：复位接收/发送标志、清空队列与回调，然后 cancel + close socket。
// cancel 使在途 async_receive_from / async_send_to 以 operation_aborted 完成，
// 其回调随后复位自己的标志并停止续发。noexcept：析构/回滚路径不能抛异常。
void UdpSocketBase::close_state(const std::shared_ptr<State>& state) noexcept
{
    state->receiving = false;
    state->send_in_flight = false;
    state->send_queue.clear();
    state->tx_queue_depth.store(0, std::memory_order_release);
    state->handler = {};

    asio::error_code ec;
    state->socket.cancel(ec); // 取消在途异步操作
    state->socket.close(ec);  // 关闭底层句柄
}

// 是否已打开：读 open 快照（bind/open 成功时由 open_and_bind 写入）。
bool UdpSocketBase::is_open() const noexcept
{
    return state_->open.load(std::memory_order_acquire);
}

// 返回 bind 成功时的本地 endpoint 快照（open_and_bind 中保存），不访问 socket，
// 因此线程安全且不会抛异常。
asio::ip::udp::endpoint UdpSocketBase::socket_local_endpoint() const noexcept
{
    return state_->local_endpoint;
}

// 聚合统计快照。计数项均为 relaxed 读，多线程下是近似值；tx_queue_depth 用
// acquire 读，保证能看到最近一次 release 写入的队列深度。
UdpTransportStats UdpSocketBase::stats() const noexcept
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
void UdpSocketBase::do_receive(const std::shared_ptr<State>& state)
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
