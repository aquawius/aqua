#include "network_client.h"
#include <spdlog/spdlog.h>

network_client::network_client(client_config cfg)
    : m_cfg(std::move(cfg))
    , m_work_guard(std::make_unique<boost::asio::io_context::work>(m_io_context))
    , m_udp_socket(m_io_context)
    , m_recv_buffer(MTU_SIZE)
{
}

network_client::~network_client()
{
    stop_client();
}

const audio_playback_linux::stream_config& network_client::get_audio_config() const
{
    if (!m_audio_playback) {
        static const audio_playback_linux::stream_config default_config {};
        return default_config;
    }
    return m_audio_playback->get_format();
}

bool network_client::init_resources()
{
    if (!setup_network()) {
        spdlog::error("[network_client] Failed to setup network");
        return false;
    }

    if (!setup_audio()) {
        spdlog::error("[network_client] Failed to setup audio");
        return false;
    }

    return true;
}

void network_client::release_resources()
{
    // 先停止所有协程
    m_running = false;

    // 断开网络连接
    disconnect_from_server();

    // 关闭音频
    if (m_audio_playback) {
        m_audio_playback->stop_playback();
        m_audio_playback.reset();
    }

    // 清理网络资源
    boost::system::error_code ec;
    m_udp_socket.close(ec);

    // 清理IO资源
    m_work_guard.reset();
    m_io_context.stop();

    // 等待IO线程结束
    if (m_io_thread.joinable()) {
        m_io_thread.join();
    }
}

bool network_client::setup_network()
{
    namespace ip = boost::asio::ip;
    boost::system::error_code ec;

    // Setup RPC client
    auto channel = grpc::CreateChannel(
        m_cfg.server_address + ":" + std::to_string(m_cfg.server_port),
        grpc::InsecureChannelCredentials());

    m_rpc_client = std::make_unique<rpc_client>(channel);

    // Setup UDP socket
    const ip::udp::endpoint local_endpoint(
        ip::make_address(m_cfg.client_address, ec),
        m_cfg.client_port);

    if (ec) {
        spdlog::error("[network_client] Invalid UDP address: {}", ec.message());
        return false;
    }

    m_udp_socket.open(local_endpoint.protocol(), ec);
    if (ec) {
        spdlog::error("[network_client] Socket open failed: {}", ec.message());
        return false;
    }

    m_udp_socket.bind(local_endpoint, ec);
    if (ec) {
        spdlog::error("[network_client] Socket bind failed: {}", ec.message());
        return false;
    }

    return true;
}

bool network_client::setup_audio()
{
    try {
        m_audio_playback = std::make_unique<audio_playback_linux>();

        if (!m_audio_playback->init()) {
            spdlog::error("[network_client] Failed to init audio playback");
            return false;
        }

        if (!m_audio_playback->setup_stream()) {
            spdlog::error("[network_client] Failed to setup audio stream");
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        spdlog::error("[network_client] Audio setup exception: {}", e.what());
        return false;
    }
}

bool network_client::start_client()
{
    if (m_running.exchange(true)) {
        spdlog::warn("[network_client] Client already running");
        return false;
    }

    if (!init_resources()) {
        m_running = false;
        return false;
    }

    if (!connect_to_server()) {
        release_resources();
        m_running = false;
        return false;
    }

    if (!m_audio_playback->start_playback()) {
        release_resources();
        m_running = false;
        return false;
    }

    // Start coroutines
    boost::asio::co_spawn(m_io_context, [this] { return udp_receive_loop(); }, boost::asio::detached);
    boost::asio::co_spawn(m_io_context, [this] { return keepalive_loop(); }, boost::asio::detached);

    // Start IO thread
    m_io_thread = std::jthread([this] {
        spdlog::debug("[network_client] IO context started");
        m_io_context.run();
        spdlog::debug("[network_client] IO context stopped");
    });

    return true;
}

bool network_client::stop_client()
{
    if (!m_running.exchange(false)) {
        return false;
    }

    spdlog::debug("[network_client] Stopping client...");
    release_resources();
    spdlog::debug("[network_client] Client stopped");
    return true;
}

bool network_client::connect_to_server()
{
    if (!m_rpc_client->connect(m_cfg.client_address, m_cfg.client_port, m_client_uuid)) {
        spdlog::error("[network_client] Failed to connect to RPC server");
        return false;
    }
    spdlog::info("[network_client] Connected with UUID: {}", m_client_uuid);
    return true;
}

void network_client::disconnect_from_server()
{
    if (!m_client_uuid.empty()) {
        m_rpc_client->disconnect(m_client_uuid);
        m_client_uuid.clear();
    }
}

void network_client::process_received_audio_data(const std::vector<uint8_t>& data_with_header)
{
    if (data_with_header.size() < AUDIO_HEADER_SIZE) {
        spdlog::warn("[network_client] Wrong packet, packet too small: {}", data_with_header.size());
        return;
    }

    // 1. 解析头部
    AudioPacketHeader header {};
    std::memcpy(&header, data_with_header.data(), AUDIO_HEADER_SIZE);

    const uint32_t received_seq = boost::endian::big_to_native(header.sequence_number);
    const uint64_t received_timestamp = boost::endian::big_to_native(header.timestamp);

    // 当前毫秒时间戳
    const auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                                           .count();

    const int64_t packet_delay = timestamp_ms - received_timestamp;
    const size_t payload_size = data_with_header.size() - AUDIO_HEADER_SIZE;
    const size_t num_samples = payload_size / sizeof(float);

    spdlog::trace("[network_client] Packet info: seq={}, timestamp={}, payload: {} samples on {} bytes, delay={}ms",
        received_seq, received_timestamp, payload_size, num_samples, packet_delay);

    // 处理音频数据
    if (num_samples == 0) {
        return;
    }

    // 序列号检查
    if (m_audio_buffer.expected_sequence != 0 && received_seq != m_audio_buffer.expected_sequence) {
        spdlog::warn("[client] Sequence gap: expected {}, got {}",
            m_audio_buffer.expected_sequence, received_seq);
        m_audio_buffer.reset();
    }

    // 添加到缓冲区
    const size_t old_size = m_audio_buffer.samples.size();
    m_audio_buffer.samples.resize(old_size + num_samples);
    std::memcpy(m_audio_buffer.samples.data() + old_size,
        data_with_header.data() + AUDIO_HEADER_SIZE,
        num_samples * sizeof(float));

    // 更新期望的下一个序列号
    m_audio_buffer.expected_sequence = received_seq + 1;

    // 更新接收的字节数统计
    m_total_bytes_received += data_with_header.size();

    if (m_audio_buffer.samples.size() >= AudioBuffer::MAX_BUFFER_SIZE) {
        process_complete_audio();
    }
}

// 在udp_receive_loop中添加定时处理逻辑
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
                    spdlog::error("[network_client] Receive error: {}", ec.message());
                }
                continue;
            }

            // 处理接收到的音频数据
            std::vector<uint8_t> received_data(m_recv_buffer.begin(), m_recv_buffer.begin() + bytes);
            process_received_audio_data(std::move(received_data));
        } catch (const std::exception& e) {
            spdlog::error("[network_client] UDP receive exception: {}", e.what());
        }
    }
}

boost::asio::awaitable<void> network_client::keepalive_loop()
{
    auto timer = boost::asio::steady_timer(m_io_context);

    while (m_running) {
        try {
            timer.expires_after(KEEPALIVE_INTERVAL);
            co_await timer.async_wait(boost::asio::use_awaitable);

            if (!is_connected()) {
                spdlog::warn("[network_client] Not connected, attempting reconnect...");
                if (!connect_to_server()) {
                    spdlog::error("[network_client] Reconnect failed");
                    stop_client();
                    break;
                }
                continue;
            }

            if (!m_rpc_client->keep_alive(m_client_uuid)) {
                spdlog::warn("[network_client] Keepalive failed, reconnecting...");
                disconnect_from_server();
                if (!connect_to_server()) {
                    spdlog::error("[network_client] Reconnect failed");
                    stop_client();
                    break;
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("[network_client] Keepalive loop exception: {}", e.what());
        }
    }
}

void network_client::process_complete_audio()
{
    if (m_audio_buffer.samples.empty()) {
        return;
    }

    // 如果日志级别高于debug，直接返回
    if (spdlog::get_level() > spdlog::level::debug) {
        return;
    }

    static char meter_buffer[41] = "----------------------------------------"; // 40个字符+结束符

    // 空数据检查
    if (m_audio_buffer.samples.empty()) {
        return;
    }

    // 计算数据的大小
    size_t size = m_audio_buffer.samples.size();

    // 只采样几个关键位置计算峰值
    float local_peak = std::max(
        std::max(std::fabs(m_audio_buffer.samples.front()), std::fabs(m_audio_buffer.samples.back())),
        std::max({
            std::fabs(m_audio_buffer.samples[size / 2]), // 中间位置
            std::fabs(m_audio_buffer.samples[size / 4]), // 1/4位置
            std::fabs(m_audio_buffer.samples[size / 8]), // 1/8位置
            std::fabs(m_audio_buffer.samples[size * 3 / 4]), // 3/4位置
            std::fabs(m_audio_buffer.samples[size * 2 / 3]) // 2/3位置
        }));

    // 更新音量条
    constexpr int METER_WIDTH = 40;
    int peak_level = std::clamp(static_cast<int>(local_peak * METER_WIDTH), 0, METER_WIDTH);

    // 清空音量条
    std::fill_n(meter_buffer, METER_WIDTH, '-');

    // 更新音量条
    if (peak_level > 0) {
        std::fill_n(meter_buffer, peak_level, '#');
    }

    spdlog::debug("[{}] {:.3f}", meter_buffer, local_peak);

    // 写入音频播放系统
    if (m_audio_playback) {
        if (!m_audio_playback->write_audio_data(m_audio_buffer.samples)) {
            spdlog::warn("[network_client] Failed to write complete audio data");
        }
    }

    // 清空缓冲区
    m_audio_buffer.reset();
}
