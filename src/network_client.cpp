// network_client.cpp
#include "network_client.h"
#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

network_client::network_client(client_config cfg)
    : m_cfg(std::move(cfg))
    , m_work_guard(std::make_unique<boost::asio::io_context::work>(m_io_context))
    , m_udp_socket(m_io_context)
    , m_recv_buffer(MAX_RECV_BUFFER_SIZE)
{
}

network_client::~network_client()
{
    stop();
}

bool network_client::setup_rpc_client()
{
    auto channel = grpc::CreateChannel(
        m_cfg.server_address + ":" + std::to_string(m_cfg.server_port),
        grpc::InsecureChannelCredentials());

    m_rpc_client = std::make_unique<rpc_client>(channel);

    if (!channel || !m_rpc_client) {
        spdlog::error("Failed to create RPC client");
        return false;
    }
    return true;
}

bool network_client::setup_udp_socket()
{
    namespace ip = boost::asio::ip;
    boost::system::error_code ec;

    const ip::udp::endpoint local_endpoint(
        ip::make_address(m_cfg.client_address, ec),
        m_cfg.client_port);

    if (ec) {
        spdlog::error("Invalid UDP address: {}", ec.message());
        return false;
    }

    m_udp_socket.open(local_endpoint.protocol(), ec);
    if (ec) {
        spdlog::error("Socket open failed: {}", ec.message());
        return false;
    }

    m_udp_socket.bind(local_endpoint, ec);
    if (ec) {
        spdlog::error("Socket bind failed: {}", ec.message());
        return false;
    }

    spdlog::debug("UDP socket bound to {}:{}",
        m_cfg.client_address, m_cfg.client_port);
    return true;
}

bool network_client::connect()
{
    if (!setup_rpc_client()) {
        return false;
    }

    static constexpr int MAX_RETRY_COUNT = 3;
    static constexpr auto RETRY_DELAY = std::chrono::seconds(1);

    int retry_count = 0;
    while (retry_count < MAX_RETRY_COUNT) {
        if (m_rpc_client->connect(m_cfg.client_address, m_cfg.client_port, m_client_uuid)) {
            spdlog::info("Registered client UUID: {}", m_client_uuid);
            return true;
        }

        retry_count++;
        if (retry_count < MAX_RETRY_COUNT) {
            spdlog::warn("Connection attempt {} failed, retrying in {} seconds...",
                retry_count, RETRY_DELAY.count());
            std::this_thread::sleep_for(RETRY_DELAY);
        }
    }

    spdlog::error("Failed to register client via RPC after {} attempts", MAX_RETRY_COUNT);
    return false;
}

void network_client::disconnect()
{
    if (!m_client_uuid.empty()) {
        m_rpc_client->disconnect(m_client_uuid);
        m_client_uuid.clear();
    }
}

bool network_client::start()
{
    if (m_running.exchange(true)) {
        spdlog::warn("Network client already running");
        return false;
    }

    if (!connect()) {
        m_running = false;
        return false;
    }

    if (!setup_udp_socket()) {
        disconnect();
        m_running = false;
        return false;
    }

    // 启动IO线程
    m_io_thread = std::jthread([this] {
        spdlog::debug("IO context started");
        m_io_context.run();
        spdlog::debug("IO context stopped");
    });

    // 启动协程
    boost::asio::co_spawn(m_io_context, [this] { return udp_receive_loop(); }, boost::asio::detached);
    boost::asio::co_spawn(m_io_context, [this] { return keepalive_loop(); }, boost::asio::detached);

    return true;
}

void network_client::stop()
{
    if (!m_running.exchange(false)) {
        return;
    }

    spdlog::debug("Stopping network client...");

    disconnect();

    boost::system::error_code ec;
    m_udp_socket.close(ec);

    m_work_guard.reset();
    m_io_context.stop();

    if (m_io_thread.joinable()) {
        m_io_thread.join();
    }

    spdlog::debug("Network client stopped");
}

boost::asio::awaitable<void> network_client::udp_receive_loop()
{
    namespace ip = boost::asio::ip;

    while (m_running) {
        try {
            ip::udp::endpoint remote_endpoint;
            const auto [ec, bytes] = co_await m_udp_socket.async_receive_from(
                boost::asio::buffer(m_recv_buffer), remote_endpoint,
                boost::asio::as_tuple(boost::asio::use_awaitable));

            if (ec) {
                if (ec != boost::asio::error::operation_aborted) {
                    spdlog::error("Receive error: {}", ec.message());
                }
                continue;
            }

            // 计算峰值
            float local_peak = 0.0f;
            const float* data = reinterpret_cast<const float*>(m_recv_buffer.data());
            size_t num_floats = bytes / sizeof(float);

            for (size_t i = 0; i < num_floats; ++i) {
                float abs_val = std::fabs(data[i]);
                if (abs_val > local_peak) {
                    local_peak = abs_val;
                }
            }
            // 简化音量条，仅显示一次整体峰值
            constexpr int peak_meter_width = 40;
            int peak_level = static_cast<int>(local_peak * peak_meter_width);
            peak_level = std::clamp(peak_level, 0, peak_meter_width);

            // 构建简化音量条并打印
            std::string meter(peak_level, '#');
            meter.resize(peak_meter_width, '-');
            spdlog::debug("[volume] [{}] {:.3f}", meter, local_peak);

        } catch (const std::exception& e) {
            spdlog::error("UDP receive exception: {}", e.what());
        }
    }
}

boost::asio::awaitable<void> network_client::keepalive_loop()
{
    auto timer = boost::asio::steady_timer(m_io_context);

    while (m_running) {
        timer.expires_after(KEEPALIVE_INTERVAL);
        co_await timer.async_wait(boost::asio::use_awaitable);

        if (!m_rpc_client->keep_alive(m_client_uuid)) {
            spdlog::warn("Keepalive failed, attempting reconnect...");
            if (!connect()) {
                spdlog::error("Reconnect failed, stopping...");
                stop();
                break;
            }
        }
    }
}
