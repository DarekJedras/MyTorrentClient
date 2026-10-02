#ifndef UDP_TRACKER_HPP
#define UDP_TRACKER_HPP

#include "asio.hpp"
#include <cstdint>
#include <vector>
#include <variant>
#include <optional>
#include "tracker.hpp"

namespace torrent::udp {

using asio_udp = asio::ip::udp;

struct ConnectRequest {
    uint64_t protocol_id = 0x41727101980; // magic constant
    uint32_t action = 0; // connect
    uint32_t transaction_id; // to be randomly generated

    std::array<char, 16> serialize() const;
};

struct ConnectResponse {
    uint32_t action;
    uint32_t transaction_id;
    uint64_t connection_id;
    std::optional<std::string> error_msg;

    // returns true if deserialization completed succesfully, false otherwise
    bool deserialize(const char* data, size_t bytes_length);
};
// sends request and process tracker response, generates request data itself
// requires socket to be opened and connected to tracker endpoint
std::optional<ConnectResponse> get_connect_response(asio_udp::socket& socket);

struct AnnounceRequest {
    uint64_t connection_id;
    uint32_t action = 1; // announce
    uint32_t transaction_id; // randomly generated
    Hash20 info_hash;
    PeerId peer_id;
    uint64_t downloaded;
    uint64_t left;
    uint64_t uploaded;
    uint32_t event = 0; // none value
    uint32_t ip_addr = 0; // default value
    uint32_t key;
    int32_t num_want = -1;
    uint16_t port;

    std::array<char, 98> serialize() const;
};

using PeersListv4 = std::vector<std::pair<uint32_t, uint16_t>>;
using PeersListv6 = std::vector<std::pair<std::array<uint8_t, 16>, uint16_t>>;

struct AnnounceResponse {
    uint32_t action;
    uint32_t transaction_id;
    uint32_t interval;
    uint32_t leechers;
    uint32_t seeders;
    std::variant<PeersListv4, PeersListv6> peers_list;
    std::optional<std::string> error_msg;

    // returns true if deserialization completed succesfully, false otherwise
    // proto has to indicate either IPv4 or IPv6 protocol
    bool deserialize(const char* data, size_t bytes_length, asio_udp proto);
};

// sends request and process tracker response, generates transaction_id itself,
// requires socket to be opened and connected to tracker endpoint
std::optional<AnnounceResponse> get_announce_response(const AnnounceRequest& request, asio_udp::socket& socket);

AnnounceResponse get_tracker_response(const ParsedURL& parsed_url, const TrackerRequest& request);
TrackerResponse parse_tracker_response(const AnnounceResponse& data);

}; // namespace torrent::udp

#endif
