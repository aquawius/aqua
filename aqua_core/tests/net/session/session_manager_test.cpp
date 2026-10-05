#include "aqua/session/session_manager.h"

#include <gtest/gtest.h>

#include <asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using aqua::session::SessionManager;
using HeartbeatOutcome = aqua::session::SessionManager::HeartbeatOutcome;

// 这些用例测的都是 IPv4 路径；族粘性/族未定另有专门用例。
constexpr auto kV4 = aqua::net::IpFamily::V4;

TEST(SessionManagerTest, CreateAndEstablishLifecycle)
{
    SessionManager manager;
    const auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());
    EXPECT_NE(*id, 0u);
    EXPECT_FALSE(manager.is_connected(*id));
    EXPECT_FALSE(manager.get_endpoint(*id).has_value());

    const auto endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 43210);
    EXPECT_EQ(manager.on_heartbeat(*id, endpoint, kV4), HeartbeatOutcome::Established);
    EXPECT_TRUE(manager.is_connected(*id));
    const auto stored = manager.get_endpoint(*id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, endpoint);
    EXPECT_TRUE(manager.remove_session(*id));
    EXPECT_FALSE(manager.is_connected(*id));
}

TEST(SessionManagerTest, InvalidEndpointsAreRejected)
{
    SessionManager manager;
    const auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());

    EXPECT_EQ(manager.on_heartbeat(
                  *id, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0), kV4),
        HeartbeatOutcome::Rejected);
    EXPECT_EQ(manager.on_heartbeat(
                  *id, asio::ip::udp::endpoint(asio::ip::address_v4::any(), 1234), kV4),
        HeartbeatOutcome::Rejected);
    EXPECT_FALSE(manager.is_connected(*id));
}

TEST(SessionManagerTest, ConcurrentCreateProducesUniqueIds)
{
    SessionManager manager;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 128;

    std::mutex mutex;
    std::atomic<bool> failed { false };
    std::vector<std::uint32_t> ids;
    ids.reserve(kThreads * kPerThread);
    std::vector<std::jthread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                const auto id = manager.create_session();
                if (!id) {
                    failed.store(true, std::memory_order_release);
                    continue;
                }
                std::lock_guard lock(mutex);
                ids.push_back(*id);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_FALSE(failed.load(std::memory_order_acquire));
    ASSERT_EQ(ids.size(), static_cast<std::size_t>(kThreads * kPerThread));
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
    EXPECT_EQ(manager.session_count(), ids.size());
}

TEST(SessionManagerTest, ExpiredSessionsAreRemovedAtomically)
{
    SessionManager manager;
    const auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());
    ASSERT_EQ(manager.on_heartbeat(
                  *id, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 40000), kV4),
        HeartbeatOutcome::Established);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto removed = manager.remove_expired_sessions(std::chrono::milliseconds(0));
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed.front(), *id);
    EXPECT_EQ(manager.session_count(), 0u);
}

TEST(SessionManagerTest, TouchRefreshesLivenessOnly)
{
    // proto Keepalive 的存活刷新：存在即更新 last_seen，不碰 endpoint/状态；
    // 不存在返回 false（调用方应停止，而非重试）。
    // 对照设计：被 touch 的活着，没被 touch 的按超时清理（2ms 阈值 vs 5ms 静置，
    // 余量与既有测试同级）。
    SessionManager manager;
    EXPECT_FALSE(manager.touch_session_liveness(0xDEADBEEFu));

    const auto id_touched = manager.create_session();
    const auto id_stale = manager.create_session();
    ASSERT_TRUE(id_touched.has_value());
    ASSERT_TRUE(id_stale.has_value());
    const auto endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 40001);
    ASSERT_EQ(manager.on_heartbeat(*id_touched, endpoint, kV4), HeartbeatOutcome::Established);
    ASSERT_EQ(manager.on_heartbeat(*id_stale, endpoint, kV4), HeartbeatOutcome::Established);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(manager.touch_session_liveness(*id_touched));

    const auto removed = manager.remove_expired_sessions(std::chrono::milliseconds(2));
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed.front(), *id_stale);
    EXPECT_EQ(manager.session_count(), 1u);
    // 存活者的 endpoint 与状态不受 touch 影响。
    EXPECT_EQ(*manager.get_endpoint(*id_touched), endpoint);
    EXPECT_TRUE(manager.is_connected(*id_touched));
}

TEST(SessionManagerTest, EstablishRefreshDoesNotTouchLiveness)
{
    // 已握手 session 的续命 heartbeat 只更新 endpoint：last_seen 是 proto
    // Keepalive 的专属领地，UDP 续命永远续不了已死的控制面。
    SessionManager manager;
    const auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());
    const auto ep1 = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 40002);
    ASSERT_EQ(manager.on_heartbeat(*id, ep1, kV4), HeartbeatOutcome::Established);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto ep2 = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 40003);
    ASSERT_EQ(manager.on_heartbeat(*id, ep2, kV4), HeartbeatOutcome::Refreshed);
    EXPECT_EQ(*manager.get_endpoint(*id), ep2); // endpoint 照常更新
    // last_seen 仍是建连时的：2ms 阈值能清掉（若被续命则清不掉）。
    const auto removed = manager.remove_expired_sessions(std::chrono::milliseconds(2));
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed.front(), *id);
}

TEST(SessionManagerTest, RoamingEndpointIsFollowedSilently)
{
    // NAT 重绑/漫游模拟（无真实 NAT 环境，用不同源端口等价代替）：
    // 同一 session_id 从新地址发 heartbeat → Refreshed，endpoint 静默跟随，
    // 状态保持 Connected，计数器 connected/refreshed 各走各的。
    SessionManager manager;
    const auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());
    const auto home = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 41001);
    const auto roaming = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 41002);

    EXPECT_EQ(manager.on_heartbeat(*id, home, kV4), HeartbeatOutcome::Established);
    EXPECT_EQ(*manager.get_endpoint(*id), home);
    // 同地址续命：同样是 Refreshed（不区分“变没变”，只区分“首包与否”）。
    EXPECT_EQ(manager.on_heartbeat(*id, home, kV4), HeartbeatOutcome::Refreshed);
    EXPECT_EQ(*manager.get_endpoint(*id), home);
    // 地址变化（client 自己感知不到，server 是唯一看见的一方）：跟随，无副作用。
    EXPECT_EQ(manager.on_heartbeat(*id, roaming, kV4), HeartbeatOutcome::Refreshed);
    EXPECT_EQ(*manager.get_endpoint(*id), roaming);
    EXPECT_TRUE(manager.is_connected(*id));

    const auto stats = manager.stats();
    EXPECT_EQ(stats.connected, 1u);
    EXPECT_EQ(stats.refreshed, 2u);
}

TEST(SessionManagerTest, UnknownSessionHeartbeatIsRejectedWithoutSideEffects)
{
    SessionManager manager;
    const auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());
    const auto endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 41003);

    // 未知 session：拒绝，且不影响已存在 session 的 endpoint/状态/计数。
    EXPECT_EQ(manager.on_heartbeat(0xDEADBEEFu, endpoint, kV4), HeartbeatOutcome::Rejected);
    EXPECT_FALSE(manager.get_endpoint(0xDEADBEEFu).has_value());
    EXPECT_EQ(manager.stats().connected, 0u);

    EXPECT_EQ(manager.on_heartbeat(*id, endpoint, kV4), HeartbeatOutcome::Established);
    EXPECT_EQ(manager.on_heartbeat(0xDEADBEEFu, endpoint, kV4), HeartbeatOutcome::Rejected);
    EXPECT_EQ(*manager.get_endpoint(*id), endpoint);
    EXPECT_TRUE(manager.is_connected(*id));
}

TEST(SessionManagerTest, PinnedFamilyRejectsCrossFamilyHeartbeat)
{
    // 族粘性（INV-3）：首包钉族，此后同 id 来自另一族的 heartbeat 一律拒绝
    // （只可能是旧 socket 残留包或伪造）。换族 = 重连 = 新 session。
    SessionManager manager;
    const auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());
    const auto v4_endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 42001);

    ASSERT_EQ(manager.on_heartbeat(*id, v4_endpoint, aqua::net::IpFamily::V4),
        HeartbeatOutcome::Established);
    const auto info = manager.get_session(*id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->family, aqua::net::IpFamily::V4);

    // endpoint 的构造与 socket 无关（不需要本机真的有 IPv6），只验证"另一族被拒"。
    const auto v6_endpoint = asio::ip::udp::endpoint(asio::ip::address_v6::loopback(), 42002);
    EXPECT_EQ(manager.on_heartbeat(*id, v6_endpoint, aqua::net::IpFamily::V6),
        HeartbeatOutcome::Rejected);
    // 原 endpoint、原族与计数都不受影响。
    EXPECT_EQ(*manager.get_endpoint(*id), v4_endpoint);
    EXPECT_EQ(manager.get_session(*id)->family, aqua::net::IpFamily::V4);
    EXPECT_EQ(manager.stats().refreshed, 0u);

    // 同族续命照常。
    EXPECT_EQ(manager.on_heartbeat(*id, v4_endpoint, aqua::net::IpFamily::V4),
        HeartbeatOutcome::Refreshed);
    EXPECT_EQ(manager.stats().refreshed, 1u);

    // 快照里的族是扇出路由的键，必须与 endpoint 一起被带出来。
    std::vector<SessionManager::ConnectedSession> connected;
    manager.snapshot_connected(connected);
    ASSERT_EQ(connected.size(), 1u);
    EXPECT_EQ(connected.front().session_id, *id);
    EXPECT_EQ(connected.front().endpoint, v4_endpoint);
    EXPECT_EQ(connected.front().family, aqua::net::IpFamily::V4);
}

TEST(SessionManagerTest, HeartbeatWithUndeterminedFamilyIsRejected)
{
    // 调用方没观测到族时不得写入：否则会话的族变成"猜的"，扇出可能选错 socket
    // （跨族发送直接 EAFNOSUPPORT），而 NAT 回包也会走错路。
    SessionManager manager;
    const auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());
    const auto endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 42003);

    EXPECT_EQ(manager.on_heartbeat(*id, endpoint, aqua::net::IpFamily::Unknown),
        HeartbeatOutcome::Rejected);
    EXPECT_FALSE(manager.is_connected(*id));
    EXPECT_EQ(manager.get_session(*id)->family, aqua::net::IpFamily::Unknown);
    EXPECT_EQ(manager.stats().connected, 0u);
}

TEST(SessionManagerTest, PendingFamilyCountsTrackTheConnectTimeFamily)
{
    // 收包健康检查要回答"这一族有没有人在等首包"：未建连的 session 真正的族仍是
    // Unknown（族只能由收到包的那条 socket 观测到），所以只能在建会话时把 Connect 期
    // 观测到的 peer 族记下来。两个计数必须**互斥**（pending 只数 Created），否则
    // "有人在等"会被重复计数。
    SessionManager manager;
    const auto id_v4 = manager.create_session(aqua::net::IpFamily::V4);
    const auto id_v6a = manager.create_session(aqua::net::IpFamily::V6);
    const auto id_v6b = manager.create_session(aqua::net::IpFamily::V6);
    const auto id_unknown = manager.create_session(); // 默认 Unknown（peer 族解析失败）
    ASSERT_TRUE(id_v4.has_value());
    ASSERT_TRUE(id_v6a.has_value());
    ASSERT_TRUE(id_v6b.has_value());
    ASSERT_TRUE(id_unknown.has_value());

    auto counts = manager.pending_family_counts();
    EXPECT_EQ(counts.v4, 1u);
    EXPECT_EQ(counts.v6, 2u);
    EXPECT_EQ(counts.unknown, 1u);

    // Connect 期记的是"期望族"；真正生效的族仍由首包落到哪条 socket 钉住（这里同为 v4）。
    const auto endpoint = asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 40000);
    ASSERT_EQ(manager.on_heartbeat(*id_v4, endpoint, aqua::net::IpFamily::V4),
        HeartbeatOutcome::Established);

    counts = manager.pending_family_counts();
    EXPECT_EQ(counts.v4, 0u) << "已建连的 session 不该再算 pending";
    EXPECT_EQ(counts.v6, 2u);
    EXPECT_EQ(counts.unknown, 1u);
    EXPECT_EQ(manager.connected_family_counts().v4, 1u);
}

} // namespace
