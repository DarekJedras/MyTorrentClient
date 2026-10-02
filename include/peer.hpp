#ifndef PEER_HPP
#define PEER_HPP

#include <string>
#include <cstdint>
#include <optional>
#include <memory>
#include <vector>
#include <functional>
#include <asio.hpp>
#include "torrent_file.hpp"

namespace torrent {

using PeerId = std::array<char, 20>;

struct PeerInfo {
    std::string ip;
    std::optional<PeerId> peer_id;
    uint16_t port;
};

enum class PeerMessageType : uint8_t {
    CHOKE = 0,
    UNCHOKE = 1,
    INTERESTED = 2,
    NOT_INTERESTED = 3,
    HAVE = 4,
    BITFIELD = 5,
    REQUEST = 6,
    PIECE = 7,
    CANCEL = 8
};

struct PeerMessage {
    std::vector<uint8_t> payload;
    PeerMessageType type;
};

using PeerRecvHandler = std::function<void(asio::error_code, PeerMessage)>;
using PeerHandshakeHandler = std::function<void(asio::error_code, Hash20)>;
using PeerErrorHandler = std::function<void(asio::error_code)>;

class Peer {
    std::optional<std::string> peer_id;
    asio::ip::tcp::socket socket;

    bool _am_chocked = true;
    bool _am_interested = false;

    bool _is_chocked = true;
    bool _is_interested = false;
public:
    bool am_interested() const;
    bool am_choked() const;
    bool is_choked() const;
    bool is_interested() const;

    void set_interested(PeerErrorHandler handler);
    void set_choked(PeerErrorHandler handler);

    void read_handshake(PeerHandshakeHandler handler);
    void send_handshake(Hash20 info_hash, Hash20 own_id);
    void send_handshake(const Hash20& info_hash, const Hash20& own_id);

    void send_message(PeerMessageType message, std::vector<uint8_t> payload);
    PeerMessageType read_message(PeerRecvHandler handler);
};

} // namespace torrent

#endif
