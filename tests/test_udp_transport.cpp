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
