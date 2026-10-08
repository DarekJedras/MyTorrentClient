#include <gtest/gtest.h>
#include "peer.hpp"
#include "asio.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace torrent {
namespace {

using torrent::Peer;
using torrent::PeerMessageType;

using tcp = asio::ip::tcp;

class PeerTest : public ::testing::Test {
protected:
    asio::io_context io;
    tcp::acceptor acceptor{io, tcp::endpoint(tcp::v4(), 0)};
    tcp::socket client{io};
    std::unique_ptr<Peer> peer;

    void connect_peer(){
        client.connect(acceptor.local_endpoint());

        tcp::socket server{io};
        acceptor.accept(server);

        peer = std::make_unique<Peer>(std::move(server));
    }

    template <typename Predicate>
    void run_until(Predicate predicate){
        while (!predicate()) {
            io.run_one();
        }
    }

    void send_handshake(const std::array<char, 20>& info_hash,
                        const std::array<char, 20>& peer_id){
        std::array<char, 68> buffer{};

        buffer[0] = 19;
        std::memcpy(buffer.data() + 1, "BitTorrent protocol", 19);

        // 8 reserved bytes = 0
        std::memcpy(buffer.data() + 28, info_hash.data(), 20);

        std::memcpy(buffer.data() + 48, peer_id.data(), 20);

        asio::write(client, asio::buffer(buffer));
    }

    void send_message(PeerMessageType type,
                      std::span<const char> payload = {}){
        const uint32_t length =
            static_cast<uint32_t>(1 + payload.size());

        std::vector<char> buffer(4 + length);

        buffer[0] = static_cast<char>((length >> 24) & 0xff);
        buffer[1] = static_cast<char>((length >> 16) & 0xff);
        buffer[2] = static_cast<char>((length >> 8) & 0xff);
        buffer[3] = static_cast<char>(length & 0xff);

        buffer[4] = std::to_underlying(type);

        std::memcpy(buffer.data() + 5, payload.data(), payload.size());

        asio::write(client, asio::buffer(buffer));
    }

    void do_handshake(){
        std::array<char, 20> info_hash{};
        std::array<char, 20> peer_id{};

        bool done = false;
        asio::error_code result;

        peer->read_handshake([&](asio::error_code ec, const auto&, const auto&, Peer&){
            result = ec;
            done = true;
        });

        send_handshake(info_hash, peer_id);

        run_until([&] {
            return done;
        });

        ASSERT_FALSE(result);
    }

    void TearDown() override{
        if (!peer)
            return;

        asio::post(io, [this] {
            peer->disconnect();
        });

        io.run();
    }
};

TEST_F(PeerTest, ReadsValidHandshake){
    connect_peer();

    std::array<char, 20> expected_hash{};
    std::array<char, 20> expected_id{};

    for (int i = 0; i < 20; ++i) {
        expected_hash[i] = static_cast<char>(i);
        expected_id[i] = static_cast<char>(20 + i);
    }

    bool done = false;
    asio::error_code result;

    peer->read_handshake([&](asio::error_code ec, const auto& hash, const auto& id, Peer&){
        result = ec;
        EXPECT_EQ(hash, expected_hash);
        EXPECT_EQ(id, expected_id);
        done = true;
    });

    send_handshake(expected_hash, expected_id);

    run_until([&] {
        return done;
    });

    EXPECT_FALSE(result);
}

TEST_F(PeerTest, RejectsInvalidHandshake){
    connect_peer();

    std::array<char, 68> buffer{};

    buffer[0] = 19;
    std::memcpy(buffer.data() + 1, "INVALID protocol", 16);

    asio::write(client, asio::buffer(buffer));

    bool done = false;
    asio::error_code result;

    peer->read_handshake([&](asio::error_code ec, const auto&, const auto&, Peer&){
        result = ec;
        done = true;
    });

    run_until([&] {
        return done;
    });

    EXPECT_EQ(result, asio::error::operation_not_supported);
}

TEST_F(PeerTest, PendingReadIsCalledWhenMessageArrives){
    connect_peer();
    do_handshake();

    bool done = false;

    peer->read_message([&](asio::error_code ec, PeerMessage msg, Peer&, bool ignored){
        EXPECT_FALSE(ec);
        EXPECT_FALSE(ignored);
        EXPECT_EQ(msg.type, PeerMessageType::UNCHOKE);
        done = true;
    });

    send_message(PeerMessageType::UNCHOKE);

    run_until([&] {
        return done;
    });
}

TEST_F(PeerTest, StateIsUpdatedWhenMessageIsRead){
    connect_peer();
    do_handshake();

    send_message(PeerMessageType::UNCHOKE);
    send_message(PeerMessageType::CHOKE);

    bool first_done = false;
    bool second_done = false;

    peer->read_message([&](asio::error_code ec, PeerMessage msg, Peer& p, bool ignored){
        ASSERT_FALSE(ec);
        ASSERT_FALSE(ignored);
        ASSERT_EQ(msg.type, PeerMessageType::UNCHOKE);

        EXPECT_FALSE(p.am_choked());

        first_done = true;
    });

    run_until([&] {
        return first_done;
    });

    peer->read_message([&](asio::error_code ec, PeerMessage msg, Peer& p, bool ignored){
        ASSERT_FALSE(ec);
        ASSERT_FALSE(ignored);
        ASSERT_EQ(msg.type, PeerMessageType::CHOKE);

        EXPECT_TRUE(p.am_choked());

        second_done = true;
    });

    run_until([&] {
        return second_done;
    });
}

TEST_F(PeerTest, RejectsSecondPendingRead){
    connect_peer();
    do_handshake();

    bool first_done = false;
    bool second_done = false;

    peer->read_message(
        [&](asio::error_code ec,
            PeerMessage,
            Peer&,
            bool ignored)
        {
            EXPECT_FALSE(ignored);
            EXPECT_FALSE(ec);
            first_done = true;
        });

    peer->read_message(
        [&](asio::error_code ec,
            PeerMessage,
            Peer&,
            bool ignored)
        {
            EXPECT_TRUE(ignored);
            EXPECT_FALSE(ec);
            second_done = true;
        });

    run_until([&] {
        return second_done;
    });

    send_message(PeerMessageType::UNCHOKE);

    run_until([&] {
        return first_done;
    });
}

TEST_F(PeerTest, PayloadHasCorrectSize){
    connect_peer();
    do_handshake();

    const std::array<char, 2> payload{'A', 'B'};

    bool done = false;

    peer->read_message([&](asio::error_code ec, PeerMessage msg, Peer&, bool ignored){
        ASSERT_FALSE(ec);
        ASSERT_FALSE(ignored);

        EXPECT_EQ(msg.length, 3);
        EXPECT_EQ(msg.type, PeerMessageType::HAVE);
        EXPECT_EQ(msg.payload.size(), 2);
        EXPECT_EQ(msg.payload[0], 'A');
        EXPECT_EQ(msg.payload[1], 'B');

        done = true;
    });

    send_message(PeerMessageType::HAVE, payload);

    run_until([&] {
        return done;
    });
}

TEST_F(PeerTest, InvalidMessageTypeSetsError){
    connect_peer();
    do_handshake();

    bool done = false;
    asio::error_code result;

    peer->read_message([&](asio::error_code ec, PeerMessage, Peer&, bool ignored){
        result = ec;
        EXPECT_FALSE(ignored);
        done = true;
    });

    // id = 9, czyli poza 0..8
    std::array<char, 5> buffer{
        0, 0, 0, 1,
        9
    };

    asio::write(client, asio::buffer(buffer));

    run_until([&] {
        return done;
    });

    EXPECT_EQ(result, asio::error::operation_not_supported);
}

TEST_F(PeerTest, UnconnectedPeerReturnsError){
    asio::io_context local_io;
    tcp::socket socket{local_io};

    Peer unconnected_peer(std::move(socket));

    bool done = false;
    asio::error_code result;

    unconnected_peer.read_message([&](asio::error_code ec, PeerMessage, Peer&, bool ignored){
        result = ec;
        EXPECT_FALSE(ignored);
        done = true;
    });

    while (!done)
        local_io.run_one();

    EXPECT_EQ(result, asio::error::not_connected);
}

} // namespace
} // namespace torrent
