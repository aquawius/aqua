#ifndef AQUA_SHARE_SESSION_MANAGER_H
#define AQUA_SHARE_SESSION_MANAGER_H

#include <asio.hpp>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <unordered_map>
#include <uuid.h>
#include <vector>

namespace aqua {

class SessionManager {
public:
    SessionManager();
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    using session_id_t = uuids::uuid;

    struct ClientInfo {
        session_id_t session_id;
        asio::ip::udp::endpoint client_endpoint;
        std::chrono::steady_clock::time_point last_seen;
    };

    session_id_t generate_session_id();

private:
    std::mt19937 random_generator_;
    uuids::uuid_random_generator uuid_generator_;
};

} // namespace aqua


#endif // AQUA_SHARE_SESSION_MANAGER_H
