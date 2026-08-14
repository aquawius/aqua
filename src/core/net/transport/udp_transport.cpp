#include "core/net/transport/udp_transport.h"

#include "core/logger/logger.h"

namespace aqua::net {

UdpTransport::UdpTransport(asio::io_context& ioc)
    : ioc_(ioc)
    , socket_(ioc)
{
}

UdpTransport::~UdpTransport()
{
    stop();
}

bool UdpTransport::bind(const std::string& bind_ip, std::uint16_t port)
{
    try {
        socket_.open(asio::ip::udp::v4());

        // reuse_address 必须在 bind 之前设置，bind 之后再设对本次绑定无效。
        socket_.set_option(asio::ip::udp::socket::reuse_address(true));

        // 显式设置内核接收缓冲区大小为 64KB。
        // Windows 默认约 8KB，高负载下易丢包；用 error_code 版本避免失败中断，
        // 仅记录 debug 便于排查。
        {
            asio::error_code rcvbuf_ec;
            socket_.set_option(
                asio::socket_base::receive_buffer_size(64 * 1024), rcvbuf_ec);
            if (rcvbuf_ec) {
                log_debug_fmt("UdpTransport set SO_RCVBUF failed on {}:{} - {}",
                              bind_ip, port, rcvbuf_ec.message());
            }
        }

        asio::ip::udp::endpoint ep(asio::ip::make_address(bind_ip), port);
        socket_.bind(ep);
        return true;
    } catch (const std::exception& e) {
        log_error_fmt("UdpTransport bind failed on {}:{} - {}", bind_ip, port, e.what());
        return false;
    }
}

void UdpTransport::start_receive(ReceiveHandler handler)
{
    handler_ = std::move(handler);
    do_receive();
}

void UdpTransport::send(const asio::ip::udp::endpoint& target,
                        std::span<const std::byte> data)
{
    // 已停止则不再投递发送，避免在 socket 关闭后访问 socket_。
    if (stopped_.load(std::memory_order_relaxed)) {
        return;
    }
    // asio socket 不是线程安全的：async_send_to 与 async_receive_from
    // 不能在不同线程并发发起。通过 post 将发送操作调度到 io_context 线程。
    auto buf = std::make_shared<std::vector<std::byte>>(data.begin(), data.end());
    asio::post(ioc_, [this, target, buf = std::move(buf)]() {
        // socket_ 只在 io_context 线程访问，安全
        socket_.async_send_to(
            asio::buffer(*buf), target,
            [buf](const asio::error_code& ec, std::size_t /*sent*/) {
                // send 失败是预期内的网络事件（如对端已关闭端口触发 ICMP
                // connection_refused），不应作为 warning 刷屏。session 的死亡
                // 判据应由 recv 超时（collect_expired_sessions）驱动，而非 send
                // 失败。这里降为 debug，便于排查但不产生噪声。
                if (ec) {
                    log_debug_fmt("UDP send failed: {}", ec.message());
                }
            });
    });
}

void UdpTransport::stop()
{
    // 已停止则跳过，避免重复关闭。
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // stopped_ 标志确保 do_receive 回调不再重新投递 async_receive_from。
    // cancel() 中止所有 pending 异步操作（handler 以 operation_aborted 触发后
    // 检查 stopped_ 直接返回）。close() 在 cancel() 之后调用，此时无 in-flight
    // 异步操作。cancel/close 的 error_code 重载是线程安全的。
    if (socket_.is_open()) {
        asio::error_code ec;
        socket_.cancel(ec);
        socket_.close(ec);
    }
}

bool UdpTransport::is_open() const noexcept
{
    return socket_.is_open();
}

asio::ip::udp::endpoint UdpTransport::socket_local_endpoint() const
{
    asio::error_code ec;
    auto ep = socket_.local_endpoint(ec);
    if (ec) {
        aqua::log_debug_fmt("UdpTransport::socket_local_endpoint error: {}", ec.message());
        return {};
    }
    return ep;
}

void UdpTransport::do_receive()
{
    socket_.async_receive_from(
        asio::buffer(recv_buf_), recv_endpoint_,
        [this](const asio::error_code& ec, std::size_t bytes) {
            if (ec) {
                // operation_aborted: socket 被 stop() 关闭，正常退出，不再投递接收。
                if (ec == asio::error::operation_aborted) {
                    return;
                }
                // 已停止则不再重新投递接收，避免在 socket 关闭流程中继续收包。
                if (stopped_.load(std::memory_order_relaxed)) {
                    return;
                }
                // 其它错误（如向已关闭的对端发包后内核回送的 ICMP port
                // unreachable / connection_refused / connection_reset）不应
                // 终止接收循环：server 仍需为其它 session 接收数据。降为 debug
                // 避免日志风暴——这些错误的根因是对端关闭，属于预期网络事件。
                log_debug_fmt("UDP recv error: {}", ec.message());
                if (socket_.is_open()) {
                    do_receive();
                }
                return;
            }
            // trace 级别输出每包来源与字节数，仅用于深度排查，正常 debug 级别不会输出。
            log_trace_fmt("UDP recv {} bytes from {}:{}",
                          bytes,
                          recv_endpoint_.address().to_string(),
                          recv_endpoint_.port());
            if (handler_) {
                handler_(recv_endpoint_,
                         std::span<const std::byte>{recv_buf_.data(), bytes});
            }
            // stopped_ 检查：stop() 可能在 handler 执行期间被调用，
            // 此时不应再投递新的 async_receive_from。
            if (stopped_.load(std::memory_order_relaxed)) {
                return;
            }
            do_receive(); // 继续接收
        });
}

} // namespace aqua::net
