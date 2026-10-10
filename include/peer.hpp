#ifndef PEER_HPP
#define PEER_HPP

#include <string>
#include <cstdint>
#include <optional>
#include <vector>
#include <functional>
#include <array>
#include <deque>
#include <span>
#include "asio.hpp"
#include "torrent_file.hpp"

namespace torrent {

using PeerId = std::array<char, 20>;

struct PeerInfo {
    std::string ip;
    std::optional<PeerId> peer_id;
    uint16_t port;
};

enum class PeerMessageType : char {
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

struct PeerMessage;

class Peer {
    // all handlers should check whether error occured and behave accordingly
public:
    using RecvHandler = std::function<void(asio::error_code, PeerMessage, Peer&, bool handler_ignored)>;
    using HandshakeHandler = std::function<void(asio::error_code, const Hash20&, const PeerId&, Peer&, bool ignored)>;
    using ErrorHandler = std::function<void(asio::error_code, Peer&, bool handler_ignored)>;

private:
    using ShortMsgBuffer = std::array<char, 20>;
    using LongMsgBuffer = std::vector<char>;
    using LongSendBuffer = std::pair<std::vector<char>, std::array<char, 5>>;
    using ReceiveBuffer = std::variant<ShortMsgBuffer, LongMsgBuffer>; // array must be first in types list
    using SendBuffer = std::variant<ShortMsgBuffer, LongSendBuffer>; // array must be first in types list
    using PendingSend = std::pair<SendBuffer, std::function<void(asio::error_code, size_t)>>;

    asio::ip::tcp::socket _socket;
    asio::error_code _ec;
    asio::steady_timer _sender_timer;
    asio::steady_timer _receiver_timer;

    std::deque<PendingSend> _send_queue;
    std::deque<ReceiveBuffer> _recv_queue;

    std::optional<RecvHandler> _pending_read;

    // queuse are unlocked after succesful handshake
    bool _send_queue_locked = true;
    bool _recv_queue_locked = true;

    bool _send_handshake_started = false;
    bool _read_handshake_started = false;

    // protocol specified state
    bool _am_choked = true;
    bool _am_interested = false;

    bool _is_choked = true;
    bool _is_interested = false;

    // designed to be called inside execution context (as tasks or callbacks)
    void store_received_msg();
    // designed to be called inside execution context (as tasks or callbacks)
    void send_stored_msg();

// all public functions are designed to be called from outside the execution context
// and delegate asynchronous task for Peer's execution context (handlers will be called asynchronously)
public:
    // socket should be constructed with executor that guarantee preserved order of callback execution
    // also socket must be connected to peer and first thing after initialization should be conducting handshake
    explicit Peer(asio::ip::tcp::socket&& socket);
    // buffers are valid during the handler invocation and owned by Peer,
    // their contents are valid only if ec == 0
    void read_handshake(HandshakeHandler handler);
    void send_handshake(const Hash20& info_hash, const Hash20& own_id, ErrorHandler handler);

    // not thread-safe, should only be called inside handlers
    bool am_interested() const {return _am_interested;}
    // not thread-safe, should only be called inside handlers
    bool am_choked() const {return _am_choked;}
    // not thread-safe, should only be called inside handlers
    bool is_choked() const {return _is_choked;}
    // not thread-safe, should only be called inside handlers
    bool is_interested() const {return _is_interested;}

    void set_interested(bool value, ErrorHandler handler);
    void set_choked(bool value, ErrorHandler handler);

    // buffers are valid during the handler invocation and owned by Peer,
    // their contents are valid only if ec == 0,
    // argument handler_ignored indicates call was invalid because other handler is pending
    void read_message(RecvHandler handler);
    // all bytes from vector will be sent, payload will be prefixed by length and message type
    void send_message(PeerMessageType message, std::vector<char> payload, ErrorHandler handler);
    // message length will be deduced from message type, payload will be prefixed by length and message type
    void send_message(PeerMessageType message, std::array<char, 12> payload, ErrorHandler handler);

    void disconnect();

// helper functions used to be called from inside
private:
    void update_internal_state(PeerMessageType type);
    void run_read_handler(const RecvHandler& handler);
    void set_keep_alive_send();
    void set_keep_alive_check();
};

// messages of length up to 16 will be stored in array (Peer::ShortMsgBuffer)
// any other will be stored in allocated vector,
// payload will contain raw message data without length prefix and message type
struct PeerMessage {
    std::span<char> payload;
    uint32_t length;
    PeerMessageType type;
};

} // namespace torrent

#endif
