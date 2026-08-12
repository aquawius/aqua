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
        asio::ip::udp::endpoint ep(asio::ip::make_address(bind_ip), port);
        socket_.bind(ep);
        socket_.set_option(asio::ip::udp::socket::reuse_address(true));
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
    // asio 要求 buffer 在异步操作完成前保持有效。
    // 异步发送需要一个持久的 buffer，因此拷贝到堆上。
    // 这是控制面频率或音频包频率，每包 ≤1500 字节，开销可接受。
    auto buf = std::make_shared<std::vector<std::byte>>(data.begin(), data.end());
    socket_.async_send_to(
        asio::buffer(*buf), target,
        [buf](const asio::error_code& ec, std::size_t /*sent*/) {
            if (ec) {
                log_warn_fmt("UDP send failed: {}", ec.message());
            }
        });
}

void UdpTransport::stop()
{
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
    return socket_.local_endpoint();
}

void UdpTransport::do_receive()
{
    socket_.async_receive_from(
        asio::buffer(recv_buf_), recv_endpoint_,
        [this](const asio::error_code& ec, std::size_t bytes) {
            if (ec) {
                if (ec != asio::error::operation_aborted) {
                    log_warn_fmt("UDP recv error: {}", ec.message());
                }
                return;
            }
            if (handler_) {
                handler_(recv_endpoint_,
                         std::span<const std::byte>{recv_buf_.data(), bytes});
            }
            do_receive(); // 继续接收
        });
}

} // namespace aqua::net
