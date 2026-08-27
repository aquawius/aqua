#include "aqua/net/udp/udp_client.h"

#include "aqua/logger/logger.h"
#include "aqua/net/udp/network_frame.h"


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
    const std::weak_ptr<State> weak_st = st;
    return st->transport->start_receive(
        [weak_st](const asio::ip::udp::endpoint& sender, std::span<const std::byte> data) {
            const auto st = weak_st.lock();
            if (!st) {
                return;
            }
            const auto expected_sender = st->transport->remote_endpoint();
            if (expected_sender.port() == 0 || sender != expected_sender) {
                return;
            }
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
    if (interval <= std::chrono::milliseconds(0)) {
        log_error("UdpClient::start_hello rejected: interval must be > 0");
        return;
    }
    if (st->hello_stopped.load(std::memory_order_acquire)) {
        return;
    }
    asio::post(st->strand, [st, session_id, interval] {
        if (st->hello_stopped.load(std::memory_order_acquire)
            || st->hello_timer != nullptr) {
            return;
        }
        if (!st->transport->has_remote()) {
            log_error("UdpClient::start_hello rejected: remote endpoint is not set");
            return;
        }
        st->hello_session_id = session_id;
        st->hello_interval = interval;
        st->hello_timer = std::make_unique<asio::steady_timer>(st->strand);
        schedule_hello(st);
    });
}

void UdpClient::stop() noexcept
{
    const auto st = state_;
    if (st->hello_stopped.exchange(true, std::memory_order_acq_rel)) {
        st->transport->stop();
        return;
    }
    try {
        asio::post(st->strand, [st] {
            if (st->hello_timer != nullptr) {
                asio::error_code ec;
                st->hello_timer->cancel(ec);
                st->hello_timer.reset();
            }
        });
    } catch (...) {
        // If posting is no longer possible, State keeps the timer alive until all
        // pending handlers/references are gone; transport stop is still performed.
    }
    st->transport->stop();
}

void UdpClient::schedule_hello(const std::shared_ptr<State>& state)
{
    if (state->hello_timer == nullptr
        || state->hello_stopped.load(std::memory_order_acquire)) {
        return;
    }
    state->hello_timer->expires_after(state->hello_interval);
    const std::weak_ptr<State> weak_state = state;
    state->hello_timer->async_wait(asio::bind_executor(state->strand,
        [weak_state](const asio::error_code& ec) {
            const auto state = weak_state.lock();
            if (!state || ec || state->hello_stopped.load(std::memory_order_acquire)) {
                return;
            }
            const auto hello = NetworkFrame::hello(state->hello_session_id).encode();
            state->transport->send(hello);
            schedule_hello(state);
        }));
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
    return state_->transport->local_endpoint();
}

UdpTransportStats UdpClient::stats() const noexcept
{
    return state_->transport->stats();
}

} // namespace aqua::net
