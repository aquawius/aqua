#ifndef AQUA_NET_UDP_PACKET_H
#define AQUA_NET_UDP_PACKET_H

#include <cstdint>

namespace aqua::net::udp {

// Aqua UDP wire-level 逻辑协议类型。具体 header 布局/编码在真正需要时再定义。
enum class PacketType : std::uint8_t {
    Invalid = 0,
    Hello = 1,
    HelloAck = 2,
    Audio = 3,
};

} // namespace aqua::net::udp

#endif // AQUA_NET_UDP_PACKET_H
