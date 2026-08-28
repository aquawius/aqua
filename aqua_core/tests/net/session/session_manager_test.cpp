#include "aqua/session/session_manager.h"

#include <gtest/gtest.h>

#include <asio.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using aqua::session::SessionManager;

TEST(SessionManagerTest, CreateAndEstablishLifecycle)
{
    SessionManager manager;
    const auto id = manager.create_session();
    ASSERT_TRUE(id.has_value());
    EXPECT_NE(*id, 0u);
    EXPECT_FALSE(manager.is_connected(*id));
    EXPECT_FALSE(manager.get_endpoint(*id).has_value());

    const auto endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 43210);
    EXPECT_TRUE(manager.establish_session(*id, endpoint));
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

    EXPECT_FALSE(manager.establish_session(
        *id, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0)));
    EXPECT_FALSE(manager.establish_session(
        *id, asio::ip::udp::endpoint(asio::ip::address_v4::any(), 1234)));
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
    std::vector<std::thread> workers;
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
    ASSERT_TRUE(manager.establish_session(
        *id, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 40000)));

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto removed = manager.remove_expired_sessions(std::chrono::milliseconds(0));
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed.front(), *id);
    EXPECT_EQ(manager.session_count(), 0u);
}

} // namespace
