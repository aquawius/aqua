#include "network_client.h"

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/endian/conversion.hpp>

#include <chrono>
#include <cstring> // std::memcpy
#include <spdlog/spdlog.h>
#include <utility>

#include "formatter.hpp"

#if defined(_WIN32) || defined(_WIN64)
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

#ifdef linux
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

network_client::network_client(client_config cfg)
    : m_client_config(std::move(cfg))
    , m_work_guard(std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(m_io_context.get_executor()))
    , m_udp_socket(m_io_context)
    , m_recv_buffer(RECV_BUFFER_SIZE)
{
    spdlog::info("Network client created with server_address={}, server_port={}, client_address={}, client_port={}",
        m_client_config.server_address,
        m_client_config.server_port,
        m_client_config.client_address,
        m_client_config.client_port);
}

network_client::~network_client()
{
    stop_client();
}

std::vector<std::string> network_client::get_address_list()
{
    std::vector<std::string> address_list;
    spdlog::trace("[network_server] Starting to enumerate network interfaces");

#if defined(_WIN32) || defined(_WIN64)
    // Windows平台使用GetAdaptersAddresses API获取网络接口信息
    ULONG family = AF_INET; // 只获取IPv4地址
    ULONG flags = GAA_FLAG_INCLUDE_ALL_INTERFACES; // 包含所有接口

    // 第一次调用获取需要的缓冲区大小
    ULONG size = 0;
    if (GetAdaptersAddresses(family, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) {
        spdlog::error("Failed to get adapter addresses buffer size");
        return address_list;
    }

    // 分配内存
    auto pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(size);
    if (!pAddresses) {
        spdlog::error("Failed to allocate memory for adapter addresses");
        return address_list;
    }

    // 获取实际的适配器信息
    auto ret = GetAdaptersAddresses(family, flags, nullptr, pAddresses, &size);
    if (ret == ERROR_SUCCESS) {
        spdlog::trace("Successfully retrieved adapter addresses from OS.");

        // 遍历所有网络适配器
        for (auto pCurrentAddress = pAddresses; pCurrentAddress; pCurrentAddress = pCurrentAddress->Next) {
            // 将宽字符适配器名称转换为普通字符串
            auto WideToMultiByte = [](const std::wstring& wstr) -> std::string {
                if (wstr.empty())
                    return std::string();

                const int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                    nullptr, 0, nullptr, nullptr);

                std::string strTo(size_needed, 0);
                WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(),
                    &strTo[0], size_needed, nullptr, nullptr);

                return strTo;
            };

            // 使用方式：
            std::wstring wAdapterName(pCurrentAddress->FriendlyName);
            std::string adapterName = WideToMultiByte(wAdapterName);

            // 检查接口是否启用
            if (pCurrentAddress->OperStatus != IfOperStatusUp) {
                spdlog::trace("Skipping interface '{}': interface is down", adapterName);
                continue;
            }

            // 跳过回环接口
            if (pCurrentAddress->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                spdlog::trace("Skipping interface '{}': loopback interface", adapterName);
                continue;
            }

            // 遍历适配器的所有单播地址
            for (auto pUnicast = pCurrentAddress->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next) {
                auto sockaddr = (sockaddr_in*)pUnicast->Address.lpSockaddr;
                char buf[INET_ADDRSTRLEN];
                // 将IP地址转换为字符串形式
                if (inet_ntop(AF_INET, &sockaddr->sin_addr, buf, sizeof(buf))) {
                    spdlog::trace("Found valid interface '{}' with address: {}", adapterName, buf);
                    address_list.emplace_back(buf);
                } else {
                    spdlog::warn("Failed to convert address to string for interface '{}'", adapterName);
                }
            }
        }
    } else {
        spdlog::error("GetAdaptersAddresses failed with error code: {}", ret);
    }

    // 清理分配的内存
    free(pAddresses);
#endif

#ifdef linux
    // Linux平台使用getifaddrs获取网络接口信息
    struct ifaddrs* ifaddrs;
    if (getifaddrs(&ifaddrs) == -1) {
        spdlog::error("[network_client] getifaddrs failed: {}", strerror(errno));
        return address_list;
    }

    spdlog::trace("[network_client] Successfully retrieved interface addresses from OS.");

    // 遍历所有网络接口
    for (auto ifa = ifaddrs; ifa; ifa = ifa->ifa_next) {
        // 检查地址是否有效
        if (!ifa->ifa_addr) {
            spdlog::trace("[network_client] Skipping interface '{}': no address", ifa->ifa_name);
            continue;
        }

        // 只处理IPv4地址
        if (ifa->ifa_addr->sa_family != AF_INET) {
            spdlog::trace("[network_client] Skipping interface '{}': not IPv4", ifa->ifa_name);
            continue;
        }

        // 跳过回环接口
        if (ifa->ifa_flags & IFF_LOOPBACK) {
            spdlog::trace("[network_client] Skipping interface '{}': loopback interface", ifa->ifa_name);
            continue;
        }

        // 检查接口是否启用
        if (!(ifa->ifa_flags & IFF_UP)) {
            spdlog::trace("[network_client] Skipping interface '{}': interface is down", ifa->ifa_name);
            continue;
        }

        // 转换IP地址为字符串形式
        auto sockaddr = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sockaddr->sin_addr, buf, sizeof(buf))) {
            spdlog::debug("[network_client] Found valid interface '{}' with address: {}", ifa->ifa_name, buf);
            address_list.emplace_back(buf);
        } else {
            spdlog::warn("[network_client] Failed to convert address to string for interface '{}'", ifa->ifa_name);
        }
    }

    // 释放接口信息
    freeifaddrs(ifaddrs);
#endif

    // 输出结果统计
    if (address_list.empty()) {
        spdlog::warn("[network_client] No valid network interfaces found");
    } else {
        spdlog::trace("[network_client] Found {} valid network interfaces:", address_list.size());
        for (const auto& addr : address_list) {
            spdlog::trace("[network_client] \t- {}", addr);
        }
    }

    return address_list;
}

/**
 * @brief 获取默认网络接口地址
 * @return 选定的默认IP地址字符串
 *
 * 地址选择优先级：
 * 1. 私有网络地址（192.168.x.x, 10.x.x.x, 172.16.x.x-172.31.x.x）
 * 2. 其他可用地址
 * 3. 如果没有可用地址，返回 0.0.0.0
 */
std::string network_client::get_default_address()
{
    // 获取所有可用地址
    auto addresses = get_address_list();
    if (addresses.empty()) {
        spdlog::warn("[network_client] No network interfaces found, using default address 0.0.0.0");
        return "0.0.0.0";
    }

    // 优先选择私有网络地址
    for (const auto& addr : addresses) {
        // 检查是否是私有网络地址
        if (addr.starts_with("192.168.") || // Class C 私有网络
            addr.starts_with("10.") || // Class A 私有网络
            addr.starts_with("172.")) { // Class B 私有网络
            spdlog::debug("[network_client] Selected private network address: {}", addr);
            return addr;
        }
    }

    // 如果没有找到私有网络地址，使用第一个可用地址
    spdlog::info("[network_client] No private network address found, using first available address: {}", addresses[0]);
    return addresses[0];
}

uint64_t network_client::get_total_bytes_received() const
{
    return m_total_bytes_received;
}

bool network_client::is_connected() const
{
    return !m_client_uuid.empty();
}

// TODO: next version: configured stream_config.
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
    m_running = false;

    // 关闭套接字和定时器
    boost::system::error_code ec;
    m_udp_socket.close(ec); // 关闭套接字会取消未完成的异步操作

    // 停止 IO 上下文
    m_work_guard.reset();
    m_io_context.stop();
    if (m_io_thread.joinable()) {
        m_io_thread.join();
    }

    // 清理其他资源（音频、RPC等）
    if (m_audio_playback) {
        m_audio_playback->stop_playback();
        m_audio_playback.reset();
    }
    disconnect_from_server();
}

bool network_client::setup_network()
{
    namespace ip = boost::asio::ip;
    boost::system::error_code ec;

    // Setup RPC client
    auto channel = grpc::CreateChannel(
        m_client_config.server_address + ":" + std::to_string(m_client_config.server_port),
        grpc::InsecureChannelCredentials());

    m_rpc_client = std::make_unique<rpc_client>(channel);

    // Setup UDP socket
    const ip::udp::endpoint local_endpoint(
        ip::make_address(m_client_config.client_address, ec),
        m_client_config.client_port);

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

    spdlog::info("[network_client] UDP bound successfully");
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

        spdlog::info("[network_client] Audio playback initialized");
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
    if (!m_rpc_client->connect(m_client_config.client_address, m_client_config.client_port, m_client_uuid)) {
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

    const auto packet_delay = static_cast<int64_t>(timestamp_ms - received_timestamp);
    const size_t payload_size = data_with_header.size() - AUDIO_HEADER_SIZE;
    const size_t num_samples = payload_size / sizeof(float);

    // if (spdlog::get_level() <= spdlog::level::trace) {
    //     spdlog::trace("[network_client] Packet info: seq={}, timestamp={}, payload: {} samples on {} bytes, delay={}ms",
    //         received_seq, received_timestamp, num_samples, payload_size, packet_delay);
    // }

    // 处理音频数据
    if (num_samples == 0) {
        spdlog::warn("[network_client] Wrong packet, no samples in packet");
        return;
    }

    if (!m_audio_playback->push_packet_data(data_with_header)) {
        spdlog::warn("[network_client] Failed to push packet #{}", received_seq);
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
            process_received_audio_data(received_data);
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

            stop_client();
            if (m_shutdown_cb) {
                m_shutdown_cb();
            }
            break;
        }
    }
}

void network_client::set_shutdown_callback(shutdown_callback cb)
{
    m_shutdown_cb = std::move(cb);
}
