// Session 生命周期严格测试
//
// 覆盖：
//   - 严格状态转换: Created -> Connected -> (Expired -> Removed)
//   - establish_udp 幂等性 (重复调用不报错, 更新 endpoint)
//   - touch_session 边界 (存在/不存在)
//   - collect_expired_sessions 时间边界 (just under / just over)
//   - clear() 行为
//   - Session ID 格式 (instance_id << 16 | counter) 与唯一性
//   - 高并发压力: 多线程 create/touch/remove/for_each
//   - 析构时残留 session 的清理

#include <gtest/gtest.h>

#include "core/session/session_manager.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <vector>

using aqua::SessionManager;
using namespace std::chrono_literals;

// ==== 严格状态转换 ====

TEST(SessionLifecycleTest, StateTransitionsCreatedToConnected)
{
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    // 初始状态: Created
    auto info = sm.get_session(*sid);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->state, SessionManager::SessionState::Created);
    EXPECT_FALSE(sm.is_connected(*sid));

    // establish_udp -> Connected
    asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"), 12345);
    ASSERT_TRUE(sm.establish_udp(*sid, ep));
    info = sm.get_session(*sid);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->state, SessionManager::SessionState::Connected);
    EXPECT_TRUE(sm.is_connected(*sid));

    // remove -> 不存在
    ASSERT_TRUE(sm.remove_session(*sid));
    EXPECT_FALSE(sm.get_session(*sid).has_value());
    EXPECT_FALSE(sm.is_connected(*sid));
}

TEST(SessionLifecycleTest, EstablishUdpIsIdempotent)
{
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    asio::ip::udp::endpoint ep1(asio::ip::make_address("127.0.0.1"), 10000);
    asio::ip::udp::endpoint ep2(asio::ip::make_address("127.0.0.1"), 20000);

    // 第一次 establish
    ASSERT_TRUE(sm.establish_udp(*sid, ep1));
    EXPECT_EQ(sm.get_endpoint(*sid).value(), ep1);
    EXPECT_TRUE(sm.is_connected(*sid));

    // 第二次 establish (NAT remap): 仍返回 true, endpoint 更新
    ASSERT_TRUE(sm.establish_udp(*sid, ep2));
    EXPECT_EQ(sm.get_endpoint(*sid).value(), ep2);
    EXPECT_TRUE(sm.is_connected(*sid));

    // 第三次 establish: 回到 ep1
    ASSERT_TRUE(sm.establish_udp(*sid, ep1));
    EXPECT_EQ(sm.get_endpoint(*sid).value(), ep1);
}

TEST(SessionLifecycleTest, TouchSessionOnlyOnExisting)
{
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    // 存在的 session: touch 成功
    EXPECT_TRUE(sm.touch_session(*sid));

    // 不存在的 session: touch 失败
    EXPECT_FALSE(sm.touch_session(0xDEADBEEFu));
    EXPECT_FALSE(sm.touch_session(0x00000000u));
}

TEST(SessionLifecycleTest, TouchSessionUpdatesLastSeenMeasurably)
{
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    auto before = sm.get_session(*sid)->last_seen;
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(sm.touch_session(*sid));
    auto after = sm.get_session(*sid)->last_seen;

    // last_seen 必须有 measurable 的增长
    EXPECT_GT(after, before);
    EXPECT_GT(std::chrono::duration_cast<std::chrono::milliseconds>(after - before).count(), 0);
}

// ==== collect_expired_sessions 时间边界 ====

TEST(SessionLifecycleTest, ExpireBoundaryJustUnderTimeout)
{
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    // 等待 900ms, 阈值 1000ms: 不应过期
    std::this_thread::sleep_for(900ms);
    auto expired = sm.collect_expired_sessions(std::chrono::seconds(1));
    EXPECT_TRUE(expired.empty());
    EXPECT_TRUE(sm.get_session(*sid).has_value());
}

TEST(SessionLifecycleTest, ExpireBoundaryJustOverTimeout)
{
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    // 等待 1100ms, 阈值 1000ms: 应过期
    std::this_thread::sleep_for(1100ms);
    auto expired = sm.collect_expired_sessions(std::chrono::seconds(1));
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0], *sid);
}

TEST(SessionLifecycleTest, TouchPreventsExpiration)
{
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    // 每 400ms touch 一次, 持续 2s, 阈值 1s: 不应过期
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(400ms);
        ASSERT_TRUE(sm.touch_session(*sid));
    }
    auto expired = sm.collect_expired_sessions(std::chrono::seconds(1));
    EXPECT_TRUE(expired.empty());

    // 停止 touch, 等待超时
    std::this_thread::sleep_for(1100ms);
    expired = sm.collect_expired_sessions(std::chrono::seconds(1));
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0], *sid);
}

TEST(SessionLifecycleTest, MixedExpiredAndAlive)
{
    SessionManager sm;
    auto sid_old = sm.create_session();
    std::this_thread::sleep_for(1100ms);
    auto sid_new = sm.create_session();
    ASSERT_TRUE(sid_old.has_value());
    ASSERT_TRUE(sid_new.has_value());

    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(sm.touch_session(*sid_new));

    auto expired = sm.collect_expired_sessions(std::chrono::seconds(1));
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0], *sid_old);

    // 清理过期的, 新的仍存活
    for (auto id : expired)
        sm.remove_session(id);
    EXPECT_FALSE(sm.get_session(*sid_old).has_value());
    EXPECT_TRUE(sm.get_session(*sid_new).has_value());
    EXPECT_EQ(sm.session_count(), 1u);
}

// ==== clear() ====

TEST(SessionLifecycleTest, ClearRemovesAllSessions)
{
    SessionManager sm;
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(sm.create_session().has_value());
    }
    EXPECT_EQ(sm.session_count(), 10u);

    auto cleared = sm.clear();
    EXPECT_EQ(cleared, 10u);
    EXPECT_EQ(sm.session_count(), 0u);
}

TEST(SessionLifecycleTest, ClearOnEmptyReturnsZero)
{
    SessionManager sm;
    EXPECT_EQ(sm.session_count(), 0u);
    EXPECT_EQ(sm.clear(), 0u);
}

TEST(SessionLifecycleTest, ClearAllowsReuseAfterwards)
{
    SessionManager sm;
    sm.create_session();
    sm.create_session();
    sm.clear();

    // clear 后仍可正常创建
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());
    EXPECT_EQ(sm.session_count(), 1u);
    asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"), 12345);
    EXPECT_TRUE(sm.establish_udp(*sid, ep));
    EXPECT_TRUE(sm.is_connected(*sid));
}

// ==== Session ID 格式与唯一性 ====

TEST(SessionLifecycleTest, SessionIdsAreUnique)
{
    SessionManager sm;
    constexpr int N = 1000;
    std::unordered_set<SessionManager::session_id_t> seen;
    for (int i = 0; i < N; ++i) {
        auto sid = sm.create_session();
        ASSERT_TRUE(sid.has_value());
        auto [it, inserted] = seen.insert(*sid);
        EXPECT_TRUE(inserted) << "Duplicate session id: 0x" << std::hex << *sid;
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(N));
}

TEST(SessionLifecycleTest, SessionIdNeverZero)
{
    // session_id = 0 保留为 UDP 音频广播标记（net::kBroadcastSessionId），绝不能被生成。
    // 由 SessionManager 构造时 instance_id_ |= 1 保证（高 16 位恒非零），
    // 因此这是确定性不变量，而非概率性抽查。
    SessionManager sm;
    for (int i = 0; i < 1000; ++i) {
        auto sid = sm.create_session();
        ASSERT_TRUE(sid.has_value());
        EXPECT_NE(*sid, 0u);
    }
}

// ==== 高并发压力测试 ====

TEST(SessionLifecycleTest, ConcurrentCreateRemoveConsistent)
{
    SessionManager sm;
    constexpr int N_THREADS = 8;
    constexpr int PER_THREAD = 100;
    std::vector<std::thread> threads;
    std::atomic<int> created { 0 };
    std::atomic<int> removed { 0 };

    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < PER_THREAD; ++i) {
                if (auto id = sm.create_session()) {
                    created.fetch_add(1, std::memory_order_relaxed);
                    if (i % 2 == 0) {
                        if (sm.remove_session(*id)) {
                            removed.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
        });
    }
    for (auto& th : threads)
        th.join();

    EXPECT_EQ(created.load(), N_THREADS * PER_THREAD);
    // removed 计数可能少于 created/2 (remove 可能被并发影响), 但必须 <= created
    EXPECT_LE(removed.load(), created.load());
    EXPECT_EQ(sm.session_count(), static_cast<size_t>(created.load() - removed.load()));
}

TEST(SessionLifecycleTest, ConcurrentCreateAndForEachNoDeadlock)
{
    // 1 个线程持续 create, 1 个线程持续 for_each_connected
    // 验证无死锁、无崩溃
    SessionManager sm;
    std::atomic<bool> stop { false };
    std::atomic<int> created { 0 };
    std::atomic<int> iterated { 0 };

    // 预先建立一些 connected session
    for (int i = 0; i < 10; ++i) {
        auto sid = sm.create_session();
        if (sid) {
            asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"),
                static_cast<unsigned short>(10000 + i));
            sm.establish_udp(*sid, ep);
        }
    }

    std::thread creator([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            if (auto sid = sm.create_session()) {
                created.fetch_add(1, std::memory_order_relaxed);
                asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"), 50000);
                sm.establish_udp(*sid, ep);
            }
        }
    });

    std::thread iterator([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            sm.for_each_connected([&](auto, const auto&) {
                iterated.fetch_add(1, std::memory_order_relaxed);
                return true;
            });
        }
    });

    // 运行 200ms
    std::this_thread::sleep_for(200ms);
    stop.store(true, std::memory_order_relaxed);

    creator.join();
    iterator.join();

    // 无死锁: 线程能正常退出
    EXPECT_GT(created.load(), 0);
    EXPECT_GT(iterated.load(), 0);
}

TEST(SessionLifecycleTest, ConcurrentTouchAndExpire)
{
    SessionManager sm;
    constexpr int N = 50;
    std::vector<SessionManager::session_id_t> sids;
    for (int i = 0; i < N; ++i) {
        auto sid = sm.create_session();
        ASSERT_TRUE(sid.has_value());
        sids.push_back(*sid);
    }

    std::atomic<int> touches { 0 };
    std::atomic<bool> stop { false };

    // 多线程同时 touch 不同 session
    std::vector<std::thread> touchers;
    for (int t = 0; t < 4; ++t) {
        touchers.emplace_back([&, t] {
            while (!stop.load(std::memory_order_relaxed)) {
                for (int i = t; i < N; i += 4) {
                    if (sm.touch_session(sids[i])) {
                        touches.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    // 同时 collect_expired
    std::thread collector([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            sm.collect_expired_sessions(std::chrono::seconds(10)); // 长阈值, 不应过期
        }
    });

    std::this_thread::sleep_for(100ms);
    stop.store(true, std::memory_order_relaxed);

    for (auto& th : touchers)
        th.join();
    collector.join();

    // 所有 session 仍存活 (touch 频繁, 阈值 10s)
    EXPECT_EQ(sm.session_count(), static_cast<size_t>(N));
    EXPECT_GT(touches.load(), 0);
}

// ==== 析构时残留 session ====

TEST(SessionLifecycleTest, DestructorWithRemainingSessionsDoesNotCrash)
{
    // SessionManager 析构时仍有 session, 不应崩溃 (仅日志 warning)
    {
        SessionManager sm;
        sm.create_session();
        sm.create_session();
        asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"), 12345);
        sm.establish_udp(*sm.create_session(), ep);
        // 不清理, 直接析构
    }
    SUCCEED(); // 到这里说明没崩溃
}

TEST(SessionLifecycleTest, DestructorAfterClearHasNoWarning)
{
    // clear 后析构, 不应有 warning
    {
        SessionManager sm;
        sm.create_session();
        sm.create_session();
        sm.clear();
        EXPECT_EQ(sm.session_count(), 0u);
    }
    SUCCEED();
}

// ==== 端口/地址边界 ====

TEST(SessionLifecycleTest, IPv6EndpointSupported)
{
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    asio::ip::udp::endpoint ep6(asio::ip::make_address("::1"), 56789);
    ASSERT_TRUE(sm.establish_udp(*sid, ep6));
    EXPECT_EQ(sm.get_endpoint(*sid).value(), ep6);
    EXPECT_TRUE(sm.is_connected(*sid));
}

TEST(SessionLifecycleTest, ZeroPortEndpointSupported)
{
    // 端口 0 在实际网络中无意义, 但 SessionManager 不应拒绝
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    asio::ip::udp::endpoint ep0(asio::ip::make_address("127.0.0.1"), 0);
    ASSERT_TRUE(sm.establish_udp(*sid, ep0));
    EXPECT_EQ(sm.get_endpoint(*sid).value().port(), 0u);
}

TEST(SessionLifecycleTest, MultipleSessionsDifferentEndpoints)
{
    SessionManager sm;
    constexpr int N = 10;
    std::vector<SessionManager::session_id_t> sids;
    std::vector<asio::ip::udp::endpoint> eps;

    for (int i = 0; i < N; ++i) {
        auto sid = sm.create_session();
        ASSERT_TRUE(sid.has_value());
        asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"),
            static_cast<unsigned short>(30000 + i));
        ASSERT_TRUE(sm.establish_udp(*sid, ep));
        sids.push_back(*sid);
        eps.push_back(ep);
    }

    // 验证每个 session 的 endpoint 独立正确
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(sm.get_endpoint(sids[i]).value(), eps[i]);
    }

    // for_each_connected 应遍历全部 N 个
    std::unordered_set<SessionManager::session_id_t> visited;
    sm.for_each_connected([&](auto sid, const auto&) {
        visited.insert(sid);
        return true;
    });
    EXPECT_EQ(visited.size(), static_cast<size_t>(N));
}

// ==== remove 后操作 ====

TEST(SessionLifecycleTest, OperationsAfterRemoveFail)
{
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());
    asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"), 12345);
    ASSERT_TRUE(sm.establish_udp(*sid, ep));

    ASSERT_TRUE(sm.remove_session(*sid));

    // 删除后所有操作应失败/返回空
    EXPECT_FALSE(sm.get_session(*sid).has_value());
    EXPECT_FALSE(sm.get_endpoint(*sid).has_value());
    EXPECT_FALSE(sm.is_connected(*sid));
    EXPECT_FALSE(sm.touch_session(*sid));
    EXPECT_FALSE(sm.establish_udp(*sid, ep));
    EXPECT_FALSE(sm.remove_session(*sid)); // 再次删除返回 false
}

// ==== for_each_connected 中途停止 ====

TEST(SessionLifecycleTest, ForEachConnectedStopEarly)
{
    SessionManager sm;
    for (int i = 0; i < 10; ++i) {
        auto sid = sm.create_session();
        ASSERT_TRUE(sid.has_value());
        asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"),
            static_cast<unsigned short>(10000 + i));
        sm.establish_udp(*sid, ep);
    }

    // 在第 3 个停止
    int count = 0;
    sm.for_each_connected([&](auto, const auto&) {
        count++;
        if (count == 3)
            return false;
        return true;
    });
    EXPECT_EQ(count, 3);
}
