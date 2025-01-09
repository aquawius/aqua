//
// Created by aquawius on 25-1-8.
//

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <asio.hpp>
#include <map>
#include <memory>

class network_manager : std::enable_shared_from_this<network_manager> {
    // 网络信令
    enum class command : uint32_t {
        cmd_none = 0,
        cmd_get_format = 1,
        cmd_start = 2,
        cmd_heartbeat = 3,
    };

    struct peer_info_t {
        int id = 0;
        asio::ip::udp::endpoint udp_peer_endpoint;
        std::chrono::steady_clock::time_point timestamp;
    };

    using players_peer_list_t = std::map<std::shared_ptr<peer_info_t>, std::shared_ptr<asio::ip::udp::endpoint>>;

public:
private:
};

#endif // NETWORK_MANAGER_H
