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

#include <regex>

#if defined(_WIN32) || defined(_WIN64)
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

#ifdef __linux__
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

network_client::network_client(std::shared_ptr<audio_playback> playback, client_config cfg)
    : m_client_config(std::move(cfg))
      , m_audio_playback(std::move(playback))
      , m_work_guard(std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(m_io_context.get_executor()))
      , m_udp_socket(m_io_context)
      , m_recv_buffer(RECV_BUFFER_SIZE)
{
    spdlog::info("Network client created with server_address={}, server_port={}, client_address={}, client_port={}",
        m_client_config.server_address,
        m_client_config.server_rpc_port,
        m_client_config.client_address,
        m_client_config.client_udp_port);
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

#ifdef __linux__
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
    auto addresses = get_address_list();
    if (addresses.empty()) {
        spdlog::warn("[network_server] No network interfaces found, using default address 0.0.0.0");
        return "0.0.0.0";
    }

    // 预编译正则表达式
    static const std::regex
        re192(R"(^192\.168\.\d+\.\d+$)"),
        re172(R"(^172\.(1[6-9]|2[0-9]|3[0-1])\.\d+\.\d+$)"),
        re10(R"(^10\.\d+\.\d+\.\d+$)");

    // 按优先级顺序检查
    for (auto&& pattern : { std::cref(re192), std::cref(re172), std::cref(re10) }) {
        for (const auto& addr : addresses) {
            if (std::regex_match(addr, pattern.get())) {
                spdlog::debug("[network_server] Selected private network address: {}", addr);
                return addr;
            }
        }
    }

    // 没有私有地址时排除回环地址
    for (const auto& addr : addresses) {
        if (addr != "127.0.0.1" && addr != "::1") {
            spdlog::info("[network_server] Using first non-loopback address: {}", addr);
            return addr;
        }
    }

    // 最后回退到默认地址
    spdlog::warn("[network_server] Fallback to loopback address");
    return "127.0.0.1";
}

uint64_t network_client::get_total_bytes_received() const
{
    return m_total_bytes_received;
}

bool network_client::is_connected() const
{
    return !m_client_uuid.empty();
}

const AudioService::auqa::pb::AudioFormat& network_client::get_server_audio_format() const
{
    // TODO: reflect
    return m_server_audio_format;
}

bool network_client::init_resources()
{
    if (!setup_network()) {
        spdlog::error("[network_client] Failed to setup network");
        return false;
    }

    return true;
}

void network_client::release_resources()
{
    spdlog::info("[network_client] Releasing all network resources...");
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

    // 清理RPC资源
    disconnect_from_server();
}

bool network_client::setup_network()
{
    namespace ip = boost::asio::ip;
    boost::system::error_code ec;

    // Setup RPC client
    auto channel = grpc::CreateChannel(
        m_client_config.server_address + ":" + std::to_string(m_client_config.server_rpc_port),
        grpc::InsecureChannelCredentials());

    m_rpc_client = std::make_unique<rpc_client>(channel);

    // Setup UDP socket
    const ip::udp::endpoint local_endpoint(
        ip::make_address(m_client_config.client_address, ec),
        m_client_config.client_udp_port);

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

    // set audio format to audio capcture.
    if (!m_audio_playback) {
        spdlog::error("[network_client] Audio playback not initialized");
        release_resources();
        m_running = false;
        return false;
    }

    if (!m_audio_playback->setup_stream(audio_common::AudioFormat(m_server_audio_format))) {
        spdlog::error("[network_client] Failed to setup audio stream");
        release_resources();
        m_running = false;
        return false;
    }

    // Start coroutines
    boost::asio::co_spawn(m_io_context, [this] { return udp_receive_loop(); }, boost::asio::detached);
    boost::asio::co_spawn(m_io_context, [this] { return keepalive_loop(); }, boost::asio::detached);
    boost::asio::co_spawn(m_io_context, [this] { return format_check_loop(); }, boost::asio::detached);

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
        spdlog::warn("[network_client] Client not running");
        return false;
    }

    spdlog::debug("[network_client] Stopping client...");
    release_resources();
    spdlog::debug("[network_client] Client stopped");
    return true;
}

bool network_client::connect_to_server()
{
    if (!m_rpc_client) {
        spdlog::error("[network_client] RPC client not initialized");
        return false;
    }

    // 向服务器发送连接请求
    if (!m_rpc_client->connect(m_client_config.client_address,
        m_client_config.client_udp_port,
        m_client_uuid,
        m_server_audio_format)) {
        spdlog::error("[network_client] Failed to connect to server");
        return false;
    }
    spdlog::info("[network_client] Connected with UUID: {}", m_client_uuid);
    spdlog::info("[network_client] Connect get server audio format: {}Hz, {}ch, encoding: {}",
        m_server_audio_format.sample_rate(),
        m_server_audio_format.channels(),
        static_cast<int>(m_server_audio_format.encoding()));

    // 服务器返回音频格式
    if (m_server_audio_format.encoding() == AudioService::auqa::pb::AudioFormat_Encoding_ENCODING_INVALID) {
        spdlog::error("[network_client] Invalid audio format");
        return false;
    }
    return true;
}

void network_client::disconnect_from_server()
{
    if (!m_client_uuid.empty()) {
        m_rpc_client->disconnect(m_client_uuid);
        m_client_uuid.clear();
    }
}

void network_client::process_received_audio_data(std::vector<std::byte>&& data_with_header)
{
    if (data_with_header.size() < AUDIO_HEADER_SIZE) {
        spdlog::warn("[network_client] Wrong packet, packet too small: {}", data_with_header.size());
        return;
    }

    // 1. 解析头部
    AudioPacketHeader header { };
    std::memcpy(&header, data_with_header.data(), AUDIO_HEADER_SIZE);

    const uint32_t received_seq = boost::endian::big_to_native(header.sequence_number);
    // const uint64_t received_timestamp = boost::endian::big_to_native(header.timestamp);

    // // 当前毫秒时间戳
    // const auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    //         std::chrono::system_clock::now().time_since_epoch())
    //     .count();
    //
    // const auto packet_delay = static_cast<int64_t>(timestamp_ms - received_timestamp);
    // const size_t payload_size = data_with_header.size() - AUDIO_HEADER_SIZE;
    //
    // if (spdlog::get_level() <= spdlog::level::trace) {
    //     spdlog::trace("[network_client] Packet info: seq={}, timestamp={}, payload: {} bytes, delay={}ms",
    //         received_seq, received_timestamp, payload_size, packet_delay);
    // }
    //

    if (!m_audio_playback) {
        spdlog::warn("[network_client] Audio playback not initialized, push packet Fail, packet #{} dropped", received_seq);
    } else {
        m_audio_playback->push_packet_data(std::move(data_with_header));
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
            std::vector<std::byte> received_data(m_recv_buffer.begin(), m_recv_buffer.begin() + bytes);
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
                spdlog::warn("[network_client] Keepalive failed, retrying...");
                bool success = false;

                for (int retry = 1; retry <= 3; ++retry) {
                    try {
                        timer.expires_after(50ms); // 复用外层定时器
                        co_await timer.async_wait(boost::asio::use_awaitable);

                        if (m_rpc_client->keep_alive(m_client_uuid)) {
                            success = true;
                            break;
                        }
                        spdlog::warn("[network_client] Keepalive failed, retry time: {}/3", retry);
                    } catch (...) {
                        break;
                    }
                }

                if (!success) {
                    spdlog::error("[network_client] Keepalive failed after 3 retries");
                    // 改为不在这里调用stop_client, asio这里会出现avoid deadlock.
                    if (m_shutdown_cb) {
                        m_shutdown_cb();
                    }
                    co_return; // 直接返回，避免后续操作
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("[network_client] Keepalive exception: {}", e.what());
            if (m_shutdown_cb) {
                m_shutdown_cb();
            }
            break;
        }
    }
}

// 定期检查音频格式更新
boost::asio::awaitable<void> network_client::format_check_loop()
{
    using namespace std::chrono_literals;
    auto timer = boost::asio::steady_timer(co_await boost::asio::this_coro::executor);

    AudioService::auqa::pb::AudioFormat server_format;

    while (m_running) {
        timer.expires_after(FORMAT_CHECK_INTERVAL);

        if (is_connected()) {
            // 获取服务器当前音频格式
            if (m_rpc_client->get_audio_format(m_client_uuid, server_format)) {
                AudioService::auqa::pb::AudioFormat current_format = m_server_audio_format;

                // 检查格式是否发生变化
                if (current_format.channels() != server_format.channels() ||
                    current_format.sample_rate() != server_format.sample_rate() ||
                    current_format.encoding() != server_format.encoding()) {

                    spdlog::info("[network_client] Detected Server audio format changed: {}Hz, {}ch, encoding: {}",
                        server_format.sample_rate(),
                        server_format.channels(),
                        static_cast<int>(server_format.encoding()));

                    m_server_audio_format = server_format;

                    // 通知音频系统重新配置
                    if (m_audio_playback) {
                        // 转换为audio_playback的格式
                        audio_playback::AudioFormat new_format(server_format);

                        // 重新配置流
                        if (!m_audio_playback->reconfigure_stream(new_format)) {
                            spdlog::error("[network_client] Failed to reconfigure audio stream");
                        }
                    }
                }
            }
        }

        co_await timer.async_wait(boost::asio::use_awaitable);
    }
}

void network_client::set_shutdown_callback(shutdown_callback cb)
{
    m_shutdown_cb = std::move(cb);
}