#include "aqua/net/udp/udp_client.h"

#include "aqua/logger/logger.h"
#include "aqua/net/udp/network_frame.h"


namespace aqua::net {

UdpClient::State::State(asio::io_context& ioc)
    : ioc(ioc)
    , transport(std::make_shared<UdpTransport>(ioc))
{
}

UdpClient::UdpClient(asio::io_context& ioc)
    : state_(std::make_shared<State>(ioc))
{
}

UdpClient::~UdpClient()
{
    stop();
}

bool UdpClient::set_remote(const std::string& server_ip, std::uint16_t port)
{
    return state_->transport->set_remote(server_ip, port);
}

bool UdpClient::start_receive(std::size_t expected_payload_bytes, FrameHandler on_frame)
{
    if (expected_payload_bytes == 0) {
        log_error("UdpClient::start_receive rejected: expected payload size is zero");
        return false;
    }
    const auto st = state_;
    if (!st->transport->is_open() && !st->transport->open()) {
        return false;
    }
    // on_frame / expected payload size 在启动接收前写入；start_receive 的 handler
    // 安装经 transport strand 串行化，写入 happens-before 任何收包回调。
    st->expected_payload_bytes = expected_payload_bytes;
    st->on_frame = std::move(on_frame);

    // 收包 handler 只捕获共享 State：即使 UdpClient 析构后 strand 上仍有
    // 排队的收包完成事件，State（及用户回调）也保持存活，无 UAF。
    return st->transport->start_receive(
        [st](const asio::ip::udp::endpoint& /*sender*/, std::span<const std::byte> data) {
            const auto frame = NetworkFrame::decode(data);
            if (!frame || frame->type() != PacketType::Audio) {
                return; // Hello/HelloAck 内部消化，malformed 丢弃
            }
            if (frame->payload().size() != st->expected_payload_bytes) {
                log_debug_fmt("UdpClient: dropping audio seq={} with payload={} bytes, expected={}",
                    frame->sequence(), frame->payload().size(), st->expected_payload_bytes);
                return;
            }
            if (st->on_frame) {
                st->on_frame(frame->sequence(), frame->payload());
            }
        });
}

void UdpClient::start_hello(std::uint32_t session_id, std::chrono::milliseconds interval)
{
    const auto st = state_;
    if (session_id == 0) {
        log_error("UdpClient::start_hello rejected: session_id is 0");
        return;
    }
    if (st->hello_timer != nullptr || st->hello_stopped.load(std::memory_order_acquire)) {
        return; // 幂等：HELLO 保活已在运行，或已停止不可复用
    }
    if (interval <= std::chrono::milliseconds(0)) {
        log_error("UdpClient::start_hello rejected: interval must be > 0");
        return;
    }
    st->hello_session_id = session_id;
    st->hello_interval = interval;
    st->hello_timer = std::make_unique<asio::steady_timer>(st->ioc);
    schedule_hello(st);
}

void UdpClient::stop() noexcept
{
    const auto st = state_;
    // 先置停止标志再取消定时器：回调链见到标志后不再续期（已取消的等待以
    // operation_aborted 完成）。不销毁定时器本身——其指针在 io 线程回调中
    // 被读取，销毁会引入跨线程竞争；定时器随 State（最后一个回调释放后）析构。
    st->hello_stopped.store(true, std::memory_order_release);
    if (st->hello_timer != nullptr) {
        asio::error_code ec;
        st->hello_timer->cancel(ec);
    }
    st->transport->stop();
}

void UdpClient::schedule_hello(const std::shared_ptr<State>& state)
{
    if (state->hello_timer == nullptr
        || state->hello_stopped.load(std::memory_order_acquire)) {
        return; // 已 stop
    }
    state->hello_timer->expires_after(state->hello_interval);
    // 回调链自续：每次到期发送一个 HELLO 并重新调度，直到 stop() 置标志取消。
    state->hello_timer->async_wait([state](const asio::error_code& ec) {
        if (ec || state->hello_stopped.load(std::memory_order_acquire)) {
            return; // cancelled（stop 流程）
        }
        const auto hello = NetworkFrame::hello(state->hello_session_id).encode();
        state->transport->send(hello);
        schedule_hello(state);
    });
}

bool UdpClient::has_remote() const noexcept
{
    return state_->transport->has_remote();
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
    return state_->transport->socket_local_endpoint();
}

UdpTransportStats UdpClient::stats() const noexcept
{
    return state_->transport->stats();
}

} // namespace aqua::net
