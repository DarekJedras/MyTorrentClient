#ifndef TRACKER_HPP
#define TRACKER_HPP

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <optional>
#include <array>
#include "peer.hpp"
#include "bencode.hpp"

namespace torrent {

struct ParsedURL {
    std::string protocol;
    std::string host;
    std::string path;
    uint16_t port;
};

enum class TrackerEvent : uint32_t {
    None = 0,
    Completed = 1,
    Started = 2,
    Stopped = 3
};

std::string_view to_string(TrackerEvent event) noexcept;

struct TrackerRequest {
    std::array<char, 20> info_hash;
    std::array<char, 20> peer_id;
    uint64_t uploaded{0};
    uint64_t downloaded{0};
    uint64_t left{0};
    TrackerEvent event{TrackerEvent::None};
    uint32_t key;
    uint16_t port;
};

struct TrackerResponse {
    long long interval = -1;
    std::vector<PeerInfo> peers;
    std::optional<std::string> failure_reason;
};

class Tracker {
    ParsedURL url_;
public:
    explicit Tracker(const std::string& announce_url);

    std::string url() const;
    TrackerResponse announce(const TrackerRequest& request) const;

    static std::vector<PeerInfo> parse_peers_list(const BencodeValue& peers, bool is_peers6 = false);
};

} // namespace torrent

#endif
