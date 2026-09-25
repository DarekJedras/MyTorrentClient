#ifndef PEER_HPP
#define PEER_HPP

#include <string>
#include <cstdint>

namespace torrent{

struct PeerInfo {
    std::string ip;
    uint16_t port;
    // std::string peer_id; // useless data
};

} // namespace torrent

#endif
