//
// Created by aquawius on 25-1-9.
//

#include "network_server.h"

#include <spdlog/spdlog.h>
#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <grpcpp/server_builder.h>

#include <chrono>
#include <cstring> // std::memcpy
#include <utility>

#include "session_manager.h"
#include "formatter.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

#ifdef linux
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#endif

namespace asio = boost::asio;
namespace ip = asio::ip;
using namespace std::chrono_literals;

std::unique_ptr<network_server> network_server::create(
    const std::string& bind_address,
    uint16_t grpc_port,
    uint16_t udp_port)
{
    auto server = std::unique_ptr<network_server>(new network_server());
    // 如果希望在 create 时就初始化各种资源，可以在此调用 init_resources：
    if (!server->init_resources(bind_address, grpc_port, udp_port)) {
        spdlog::error("Failed to initialize network resources in create().");
        return nullptr;
    }
    return server;
}

network_server::network_server()
{
    spdlog::debug("network_server constructor called.");
}

network_server::~network_server()
{
    stop_server(); // 确保析构前停止服务并释放资源
    spdlog::debug("network_server destructor called.");
}

bool network_server::init_resources(const std::string& addr, uint16_t grpc_port, uint16_t udp_port)
{
    if (m_is_running) {
        spdlog::warn("Server is already running. init_resources() will be ignored.");
        return false;
    }

    // 若未指定 addr 则获取默认地址
    std::string local_address = addr.empty() ? get_default_address() : addr;
    spdlog::info("Initializing network_server on address {} (gRPC port: {}, UDP port: {})",
        local_address, grpc_port, udp_port);

    try {
        // 1. 创建并持有 IO 上下文
        m_io_context = std::make_unique<asio::io_context>();
        m_work_guard = std::make_unique<asio::io_context::work>(*m_io_context);

        // 2. 创建并绑定 UDP 套接字
        m_udp_socket = std::make_unique<udp_socket>(*m_io_context);
        ip::udp::endpoint local_endpoint(ip::make_address(local_address), udp_port);
        m_udp_socket->open(local_endpoint.protocol());
        m_udp_socket->set_option(ip::udp::socket::reuse_address(true));
        m_udp_socket->bind(local_endpoint);

        spdlog::info("UDP socket bound to {}:{}", local_address, udp_port);

        // 3. 构建并启动 gRPC Server
        grpc::ServerBuilder builder;
        std::string server_address = local_address + ":" + std::to_string(grpc_port);
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

        // 如果需要注册更多服务，按需增加
        m_rpc_server = std::make_unique<RPCServer>(*this);
        builder.RegisterService(m_rpc_server.get());

        m_grpc_server = builder.BuildAndStart();
        if (!m_grpc_server) {
            spdlog::error("Failed to build gRPC server on address: {}", server_address);
            return false;
        }
        spdlog::info("gRPC server listening on {}", server_address);

        return true;
    } catch (const std::exception& e) {
        spdlog::error("Exception in init_resources(): {}", e.what());
        return false;
    }
}
void network_server::release_resources()
{
    spdlog::info("Releasing all network resources...");

    // 1. 关闭 gRPC
    if (m_grpc_server) {
        m_grpc_server->Shutdown();
        m_grpc_server.reset();
    }

    // 2. 关闭 UDP socket
    if (m_udp_socket && m_udp_socket->is_open()) {
        boost::system::error_code ec;
        m_udp_socket->close(ec);
        if (ec) {
            spdlog::warn("Error closing UDP socket: {}", ec.message());
        }
        m_udp_socket.reset();
    }

    // 3. 停止 IO 上下文
    if (m_io_context) {
        m_io_context->stop();
    }

    // 4. 等待所有 IO 线程退出并清理
    for (auto& th : m_io_threads) {
        if (th.joinable()) {
            th.join();
        }
    }
    m_io_threads.clear();

    // 5. 等待所有 gRPC 线程退出并清理
    for (auto& th : m_grpc_threads) {
        if (th.joinable()) {
            th.join();
        }
    }
    m_grpc_threads.clear();

    // 6. 清理 WorkGuard 与 IOContext
    m_work_guard.reset();
    m_io_context.reset();

    spdlog::info("All network resources have been released.");
}

std::vector<std::string> network_server::get_address_list()
{
    std::vector<std::string> address_list;
    spdlog::debug("Starting to enumerate network interfaces");

#ifdef _WINDOWS
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
        spdlog::debug("Successfully retrieved adapter addresses");

        // 遍历所有网络适配器
        for (auto pCurrentAddress = pAddresses; pCurrentAddress; pCurrentAddress = pCurrentAddress->Next) {
            // 将宽字符适配器名称转换为普通字符串
            std::wstring wAdapterName(pCurrentAddress->FriendlyName);
            std::string adapterName(wAdapterName.begin(), wAdapterName.end());

            // 检查接口是否启用
            if (pCurrentAddress->OperStatus != IfOperStatusUp) {
                spdlog::debug("Skipping interface '{}': interface is down", adapterName);
                continue;
            }

            // 跳过回环接口
            if (pCurrentAddress->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                spdlog::debug("Skipping interface '{}': loopback interface", adapterName);
                continue;
            }

            // 遍历适配器的所有单播地址
            for (auto pUnicast = pCurrentAddress->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next) {
                auto sockaddr = (sockaddr_in*)pUnicast->Address.lpSockaddr;
                char buf[INET_ADDRSTRLEN];
                // 将IP地址转换为字符串形式
                if (inet_ntop(AF_INET, &sockaddr->sin_addr, buf, sizeof(buf))) {
                    spdlog::debug("Found valid interface '{}' with address: {}", adapterName, buf);
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
        spdlog::error("getifaddrs failed: {}", strerror(errno));
        return address_list;
    }

    spdlog::debug("Successfully retrieved interface addresses");

    // 遍历所有网络接口
    for (auto ifa = ifaddrs; ifa; ifa = ifa->ifa_next) {
        // 检查地址是否有效
        if (!ifa->ifa_addr) {
            spdlog::debug("Skipping interface '{}': no address", ifa->ifa_name);
            continue;
        }

        // 只处理IPv4地址
        if (ifa->ifa_addr->sa_family != AF_INET) {
            spdlog::debug("Skipping interface '{}': not IPv4", ifa->ifa_name);
            continue;
        }

        // 跳过回环接口
        if (ifa->ifa_flags & IFF_LOOPBACK) {
            spdlog::debug("Skipping interface '{}': loopback interface", ifa->ifa_name);
            continue;
        }

        // 检查接口是否启用
        if (!(ifa->ifa_flags & IFF_UP)) {
            spdlog::debug("Skipping interface '{}': interface is down", ifa->ifa_name);
            continue;
        }

        // 转换IP地址为字符串形式
        auto sockaddr = (sockaddr_in*)ifa->ifa_addr;
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sockaddr->sin_addr, buf, sizeof(buf))) {
            spdlog::debug("Found valid interface '{}' with address: {}", ifa->ifa_name, buf);
            address_list.emplace_back(buf);
        } else {
            spdlog::warn("Failed to convert address to string for interface '{}'", ifa->ifa_name);
        }
    }

    // 释放接口信息
    freeifaddrs(ifaddrs);
#endif

    // 输出结果统计
    if (address_list.empty()) {
        spdlog::warn("No valid network interfaces found");
    } else {
        spdlog::info("Found {} valid network interfaces:", address_list.size());
        for (const auto& addr : address_list) {
            spdlog::info("  - {}", addr);
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
std::string network_server::get_default_address()
{
    // 获取所有可用地址
    auto addresses = get_address_list();
    if (addresses.empty()) {
        spdlog::warn("No network interfaces found, using default address 0.0.0.0");
        return "0.0.0.0";
    }

    // 优先选择私有网络地址
    for (const auto& addr : addresses) {
        // 检查是否是私有网络地址
        if (addr.starts_with("192.168.") || // Class C 私有网络
            addr.starts_with("10.") || // Class A 私有网络
            addr.starts_with("172.")) { // Class B 私有网络
            spdlog::info("Selected private network address: {}", addr);
            return addr;
        }
    }

    // 如果没有找到私有网络地址，使用第一个可用地址
    spdlog::info("No private network address found, using first available address: {}", addresses[0]);
    return addresses[0];
}

bool network_server::start_server()
{
    if (m_is_running) {
        spdlog::warn("Server is already running. start_server() will be ignored.");
        return false;
    }

    // 若此时还未初始化资源，可在这里再做一次检查或直接调用 init_resources()

    m_is_running = true;

    // 1. 启动若干个 IO 线程
    constexpr unsigned int thread_count = 2;
    for (unsigned int i = 0; i < thread_count; ++i) {
        m_io_threads.emplace_back([this]() {
            spdlog::info("IO thread started, thread_id={}.", std::this_thread::get_id());
            m_io_context->run();
            spdlog::info("IO thread stopped, thread_id={}.", std::this_thread::get_id());
        });
    }

    // 2. 启动 gRPC 工作线程
    m_grpc_threads.emplace_back([this]() {
        spdlog::info("gRPC thread started, thread_id={}.", std::this_thread::get_id());
        m_grpc_server->Wait(); // 阻塞等待
        spdlog::info("gRPC thread stopped, thread_id={}.", std::this_thread::get_id());
    });

    // 3. 协程发送音频和定期检查会话
    asio::co_spawn(*m_io_context, handle_udp_send(), asio::detached);
    asio::co_spawn(*m_io_context, check_sessions_routine(), asio::detached);

    spdlog::info("Server started successfully.");
    return true;
}

bool network_server::stop_server()
{
    if (!m_is_running) {
        spdlog::warn("Server is not running. stop_server() will be ignored.");
        return false;
    }

    spdlog::info("Stopping server...");
    m_is_running = false;

    release_resources();
    spdlog::info("Server stopped successfully.");
    return true;
}

void network_server::push_audio_data(std::span<const float> audio_data)
{
    // 1. 将浮点音频数据转换为字节缓冲
    std::vector<uint8_t> buffer(audio_data.size() * sizeof(float));
    std::memcpy(buffer.data(), audio_data.data(), buffer.size());

    // 2. 入队，若队列满则丢弃最早的数据
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_send_queue.size() >= MAX_SEND_QUEUE_SIZE) {
            m_send_queue.pop_front();
            spdlog::warn("Send queue is full, discarding oldest packet.");
        }
        audio_packet pkt;
        pkt.data = std::move(buffer);
        pkt.timestamp = std::chrono::steady_clock::now();
        m_send_queue.push_back(std::move(pkt));
    }
}

asio::awaitable<void> network_server::handle_udp_send()
{
    using asio::steady_timer;

    while (m_is_running) {
        std::vector<uint8_t> packet;
        bool has_packet = false;

        // 1. 取队列中一份待发送音频包
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            if (!m_send_queue.empty()) {
                packet = std::move(m_send_queue.front().data);
                m_send_queue.pop_front();
                has_packet = true;
            }
        }

        // 2. 如果有包，发送给所有活跃客户端；否则小憩一会儿
        if (has_packet) {
            auto endpoints = session_manager::get_instance().get_active_endpoints();
            for (const auto& ep : endpoints) {
                boost::system::error_code ec;
                std::size_t bytes_sent = co_await m_udp_socket->async_send_to(
                    asio::buffer(packet), ep,
                    asio::redirect_error(asio::use_awaitable, ec));

                if (!ec && bytes_sent > 0) {
                    m_total_bytes_sent += bytes_sent;
                } else if (ec) {
                    spdlog::debug("UDP send error: {} -> {} ({} bytes attempted)",
                        ec.message(), ep.address().to_string(), packet.size());
                }
            }
        } else {
            steady_timer timer(m_udp_socket->get_executor());
            timer.expires_after(10ms);
            co_await timer.async_wait(asio::use_awaitable);
        }
    }

    co_return;
}

asio::awaitable<void> network_server::check_sessions_routine()
{
    using asio::steady_timer;

    while (m_is_running) {
        steady_timer timer(m_udp_socket->get_executor());
        timer.expires_after(1s);
        co_await timer.async_wait(asio::use_awaitable);

        session_manager::get_instance().check_sessions();
    }

    co_return;
}
