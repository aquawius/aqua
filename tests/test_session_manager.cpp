#include <gtest/gtest.h>

#include "core/session/session_manager.h"

#include <thread>

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
