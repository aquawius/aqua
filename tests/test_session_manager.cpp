#include <gtest/gtest.h>

#include "core/session/session_manager.h"

#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;

TEST(SessionManagerTest, CreateSessionReturnsValidId)
{
    aqua::SessionManager manager;
    auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());
    EXPECT_NE(id.value(), 0u);

    auto info = manager.get_session(id.value());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->session_id, id.value());
    EXPECT_EQ(info->state, aqua::SessionManager::SessionState::Created);
}

TEST(SessionManagerTest, RemoveSession)
{
    aqua::SessionManager manager;
    auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());

    EXPECT_TRUE(manager.remove_session(id.value()));
    EXPECT_FALSE(manager.get_session(id.value()).has_value());
    EXPECT_FALSE(manager.remove_session(id.value()));
}

TEST(SessionManagerTest, EstablishUdpMarksConnected)
{
    aqua::SessionManager manager;
    auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());

    asio::ip::udp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), 12345);
    EXPECT_TRUE(manager.establish_udp(id.value(), endpoint));

    auto retrieved = manager.get_endpoint(id.value());
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved.value(), endpoint);

    auto info = manager.get_session(id.value());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->state, aqua::SessionManager::SessionState::Connected);
    EXPECT_TRUE(manager.is_connected(id.value()));

    asio::ip::udp::endpoint other(asio::ip::make_address("127.0.0.1"), 54321);
    EXPECT_FALSE(manager.establish_udp(0u, other));
}

TEST(SessionManagerTest, TouchSessionUpdatesLastSeen)
{
    aqua::SessionManager manager;
    auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());

    auto before = manager.get_session(id.value())->last_seen;
    std::this_thread::sleep_for(20ms);
    EXPECT_TRUE(manager.touch_session(id.value()));
    auto after = manager.get_session(id.value())->last_seen;

    EXPECT_GT(after, before);
    EXPECT_FALSE(manager.touch_session(0u));
}

TEST(SessionManagerTest, CollectExpiredSessions)
{
    aqua::SessionManager manager;
    auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());

    std::this_thread::sleep_for(1500ms);
    auto expired = manager.collect_expired_sessions(1s);

    ASSERT_EQ(expired.size(), 1);
    EXPECT_EQ(expired[0], id.value());
}

TEST(SessionManagerTest, NonExpiredSessionIsNotCollected)
{
    aqua::SessionManager manager;
    auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());

    std::this_thread::sleep_for(50ms);
    auto expired = manager.collect_expired_sessions(10s);

    EXPECT_TRUE(expired.empty());
}

TEST(SessionManagerTest, SessionCount)
{
    aqua::SessionManager manager;
    EXPECT_EQ(manager.session_count(), 0);

    auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(manager.session_count(), 1);

    manager.remove_session(id.value());
    EXPECT_EQ(manager.session_count(), 0);
}

// ---- for_each_connected ----

TEST(SessionManagerTest, ForEachConnectedEmpty)
{
    aqua::SessionManager manager;
    size_t count = 0;
    manager.for_each_connected([&](auto, const auto&) { count++; return true; });
    EXPECT_EQ(count, 0u);
}

TEST(SessionManagerTest, ForEachConnectedSkipsNonConnected)
{
    aqua::SessionManager manager;
    // 1 个 Created (未握手), 1 个 Connected
    auto id_created = manager.create_session();
    auto id_connected = manager.create_session();
    ASSERT_TRUE(id_created.has_value());
    ASSERT_TRUE(id_connected.has_value());

    asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"), 5000);
    ASSERT_TRUE(manager.establish_udp(id_connected.value(), ep));

    std::vector<aqua::SessionManager::session_id_t> visited;
    manager.for_each_connected([&](auto id, const auto&) {
        visited.push_back(id);
        return true;
    });
    ASSERT_EQ(visited.size(), 1u);
    EXPECT_EQ(visited[0], id_connected.value());
}

TEST(SessionManagerTest, ForEachConnectedStopsOnFalse)
{
    aqua::SessionManager manager;
    // 创建 3 个 connected session
    std::vector<aqua::SessionManager::session_id_t> ids;
    for (int i = 0; i < 3; ++i) {
        auto id = manager.create_session();
        ASSERT_TRUE(id.has_value());
        asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"),
                                    static_cast<unsigned short>(10000 + i));
        ASSERT_TRUE(manager.establish_udp(*id, ep));
        ids.push_back(*id);
    }

    size_t visited = 0;
    manager.for_each_connected([&](auto, const auto&) {
        visited++;
        return false; // 第一次就停止
    });
    EXPECT_EQ(visited, 1u);
}

TEST(SessionManagerTest, ForEachConnectedVisitsAll)
{
    aqua::SessionManager manager;
    for (int i = 0; i < 5; ++i) {
        auto id = manager.create_session();
        ASSERT_TRUE(id.has_value());
        asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"),
                                    static_cast<unsigned short>(20000 + i));
        ASSERT_TRUE(manager.establish_udp(*id, ep));
    }

    size_t visited = 0;
    manager.for_each_connected([&](auto, const auto&) { visited++; return true; });
    EXPECT_EQ(visited, 5u);
}

// ---- re-establish / NAT remap ----

TEST(SessionManagerTest, ReEstablishUdpUpdatesEndpoint)
{
    // 模拟 NAT 重映射：client 重新发 HELLO，endpoint 变化
    aqua::SessionManager manager;
    auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());

    asio::ip::udp::endpoint ep1(asio::ip::make_address("127.0.0.1"), 30000);
    ASSERT_TRUE(manager.establish_udp(*id, ep1));
    EXPECT_EQ(manager.get_endpoint(*id).value(), ep1);

    // NAT remap: 新端口
    asio::ip::udp::endpoint ep2(asio::ip::make_address("127.0.0.1"), 39999);
    ASSERT_TRUE(manager.establish_udp(*id, ep2));
    EXPECT_EQ(manager.get_endpoint(*id).value(), ep2);
    EXPECT_NE(manager.get_endpoint(*id).value(), ep1);

    // 状态仍为 Connected
    EXPECT_TRUE(manager.is_connected(*id));
}

TEST(SessionManagerTest, EstablishUdpOnUnknownSessionFails)
{
    aqua::SessionManager manager;
    asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"), 40000);
    // 0xFFFFFFFF 几乎必然不存在
    EXPECT_FALSE(manager.establish_udp(0xFFFFFFFFu, ep));
}

// ---- collect_expired 混合场景 ----

TEST(SessionManagerTest, CollectExpiredMixed)
{
    aqua::SessionManager manager;
    auto id1 = manager.create_session();      // 会过期
    std::this_thread::sleep_for(1100ms);
    auto id2 = manager.create_session();      // 新建, 不会过期
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(id2.has_value());

    // id2 触摸一下, 刷新 last_seen
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(manager.touch_session(id2.value()));

    // 阈值 1s: id1 (>1.1s) 过期, id2 (刚 touch) 不过期
    auto expired = manager.collect_expired_sessions(std::chrono::seconds(1));
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0], id1.value());
}

// ---- 并发安全性 ----

TEST(SessionManagerTest, ConcurrentCreateRemove)
{
    aqua::SessionManager manager;
    constexpr int N = 8;
    constexpr int per_thread = 50;
    std::vector<std::thread> threads;
    std::atomic<int> created{0};

    for (int t = 0; t < N; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < per_thread; ++i) {
                if (auto id = manager.create_session()) {
                    created.fetch_add(1, std::memory_order_relaxed);
                    // 立即删除一半
                    if (i % 2 == 0) {
                        manager.remove_session(*id);
                    }
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(created.load(), N * per_thread);
    // 创建了 N*per_thread 个, 删除了一半 (i%2==0 -> per_thread/2 个/线程)
    const size_t expected_remaining = N * (per_thread - per_thread / 2);
    EXPECT_EQ(manager.session_count(), expected_remaining);
}

TEST(SessionManagerTest, SessionIdsAreUnique)
{
    aqua::SessionManager manager;
    constexpr int N = 500;
    std::unordered_map<aqua::SessionManager::session_id_t, int> seen;

    for (int i = 0; i < N; ++i) {
        auto id = manager.create_session();
        ASSERT_TRUE(id.has_value());
        seen[*id]++;
    }
    EXPECT_EQ(seen.size(), N); // 无重复
}

// ---- 端口/地址边界 ----

TEST(SessionManagerTest, EstablishUdpIPv6Endpoint)
{
    // 确保 IPv6 endpoint 也能正常记录
    aqua::SessionManager manager;
    auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());

    asio::ip::udp::endpoint ep6(asio::ip::make_address("::1"), 12345);
    ASSERT_TRUE(manager.establish_udp(*id, ep6));
    EXPECT_EQ(manager.get_endpoint(*id).value(), ep6);
    EXPECT_TRUE(manager.is_connected(*id));
}
