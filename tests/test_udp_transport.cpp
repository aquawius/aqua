#include <gtest/gtest.h>

#include "core/net/transport/udp_transport.h"

#include <atomic>
#include <thread>

using aqua::net::UdpTransport;

TEST(UdpTransportTest, BindAndClose)
{
    asio::io_context ioc;
    UdpTransport transport(ioc);
    EXPECT_TRUE(transport.bind("127.0.0.1", 0)); // 随机端口
    EXPECT_TRUE(transport.is_open());
    transport.stop();
    EXPECT_FALSE(transport.is_open());
}

TEST(UdpTransportTest, BindFailsOnBadIp)
{
    asio::io_context ioc;
    UdpTransport transport(ioc);
    EXPECT_FALSE(transport.bind("999.999.999.999", 0));
}

TEST(UdpTransportTest, SendReceiveLoopback)
{
    asio::io_context ioc;

    UdpTransport receiver(ioc);
    ASSERT_TRUE(receiver.bind("127.0.0.1", 0));

    // 获取实际绑定端口
    auto local_ep = receiver.socket_local_endpoint();
    ASSERT_NE(local_ep.port(), 0u);

    std::atomic<bool> received{false};
    std::vector<std::byte> recv_data;

    receiver.start_receive([&](const auto& /*sender*/, std::span<const std::byte> data) {
        recv_data.assign(data.begin(), data.end());
        received = true;
    });

    // 发送端
    UdpTransport sender(ioc);
    ASSERT_TRUE(sender.bind("127.0.0.1", 0));

    std::vector<std::byte> msg = {std::byte{1}, std::byte{2}, std::byte{3}};
    sender.send(local_ep, msg);

    // 运行 io_context 直到收到
    std::thread t([&] { ioc.run(); });
    // 等待接收
    for (int i = 0; i < 100 && !received; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_TRUE(received);
    ASSERT_EQ(recv_data.size(), 3u);
    EXPECT_EQ(recv_data[0], std::byte{1});
    EXPECT_EQ(recv_data[1], std::byte{2});
    EXPECT_EQ(recv_data[2], std::byte{3});

    ioc.stop();
    t.join();
}

TEST(UdpTransportTest, MultiplePackets)
{
    asio::io_context ioc;
    UdpTransport receiver(ioc);
    ASSERT_TRUE(receiver.bind("127.0.0.1", 0));
    auto local_ep = receiver.socket_local_endpoint();

    std::atomic<int> count{0};
    receiver.start_receive([&](const auto&, auto) {
        count++;
    });

    UdpTransport sender(ioc);
    ASSERT_TRUE(sender.bind("127.0.0.1", 0));

    std::vector<std::byte> msg(10, std::byte{0xAA});
    for (int i = 0; i < 5; ++i)
        sender.send(local_ep, msg);

    std::thread t([&] { ioc.run(); });
    for (int i = 0; i < 200 && count < 5; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_GE(count.load(), 1);

    ioc.stop();
    t.join();
}

// 回归测试: 模拟 server 向已关闭的 client 端口发包触发 ICMP port unreachable,
// receiver 收到 connection_refused 错误后, 必须仍能接收后续包。
// 复现场景: client 非正常退出后, server 第二次 client 连接收不到 HELLO。
TEST(UdpTransportTest, RecoversFromConnectionRefusedError)
{
    asio::io_context ioc;

    UdpTransport receiver(ioc);
    ASSERT_TRUE(receiver.bind("127.0.0.1", 0));
    auto receiver_ep = receiver.socket_local_endpoint();

    std::atomic<int> received_count{0};
    receiver.start_receive([&](const auto&, auto) {
        received_count.fetch_add(1, std::memory_order_relaxed);
    });

    UdpTransport sender(ioc);
    ASSERT_TRUE(sender.bind("127.0.0.1", 0));

    std::thread ioc_thread([&] { ioc.run(); });

    // 步骤 1: 构造一个"已关闭的端口"作为目标, 让 receiver 向它发包,
    //         触发本机 ICMP port unreachable -> receiver 的 async_receive_from
    //         返回 connection_refused 错误。
    {
        UdpTransport dead_client(ioc);
        ASSERT_TRUE(dead_client.bind("127.0.0.1", 0));
        auto dead_ep = dead_client.socket_local_endpoint();
        // dead_client 此处析构, 端口关闭
        std::vector<std::byte> msg = {std::byte{0x01}};
        receiver.send(dead_ep, msg);
        // 等待 ICMP 回送 (本机回环很快)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 步骤 2: 向 receiver 发一个正常包, 验证接收循环已恢复
    std::vector<std::byte> normal_msg = {std::byte{0x42}, std::byte{0x43}};
    sender.send(receiver_ep, normal_msg);

    for (int i = 0; i < 100 && received_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 修复前: received_count == 0 (接收循环已死)
    // 修复后: received_count >= 1 (接收循环恢复)
    EXPECT_GE(received_count.load(), 1);

    ioc.stop();
    ioc_thread.join();
}
