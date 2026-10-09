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
        io.restart();
        asio::steady_timer timer(io);
        bool timed_out = false;

        timer.expires_after(std::chrono::milliseconds(50));

        timer.async_wait([&](asio::error_code ec){
                ASSERT_TRUE(!ec || ec == asio::error::operation_aborted);
                timed_out = true;
        });

        while (!predicate() && !timed_out){
            io.run_one();
        }

        if (!timed_out){
            timer.cancel();
            io.poll();
        } else {
            FAIL() << "Timed out waiting for condition";
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

    void receive_handshake_from_peer(){
        std::array<char, 20> info_hash{};
        std::array<char, 20> own_id{};

        bool sent = false, read = false;

        peer->send_handshake(info_hash, own_id, [&](asio::error_code ec, Peer&, bool was_ignored) {
            ASSERT_FALSE(ec);
            ASSERT_FALSE(was_ignored);
            sent = true;
        });

        std::array<char, 68> received{};
        asio::async_read(client, asio::buffer(received), [&](asio::error_code ec, size_t){
            ASSERT_FALSE(ec);
            read = true;
        });

        run_until([&] { return sent && read; });
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

        bool read_done = false;

        peer->read_handshake([&](asio::error_code ec, const auto&, const auto&, Peer&, bool ignored){
            ASSERT_FALSE(ec);
            ASSERT_FALSE(ignored);
            read_done = true;
        });

        send_handshake(info_hash, peer_id);

        peer->send_handshake({}, {}, {});

        bool send_done = false;
        std::array<char, 68> buf;
        asio::async_read(client, asio::buffer(buf), [&](asio::error_code ec, size_t){
            ASSERT_FALSE(ec);
            send_done = true;
        });

        run_until([&]{
            return read_done && send_done;
        });
    }

    void TearDown() override{
        if (!peer)
            return;

        asio::post(io, [this]{
            peer->disconnect();
        });

        io.run();
    }
};

// ============================================================================
// READ TESTS
// ============================================================================


TEST_F(PeerTest, ReadsValidHandshake){
    connect_peer();

    std::array<char, 20> expected_hash{};
    std::array<char, 20> expected_id{};

    for (int i = 0; i < 20; ++i) {
        expected_hash[i] = static_cast<char>(i);
        expected_id[i] = static_cast<char>(20 + i);
    }

    bool done = false;

    peer->read_handshake([&](asio::error_code ec, const auto& hash, const auto& id, Peer&, bool ignored){
        EXPECT_FALSE(ignored);
        EXPECT_FALSE(ec);
        EXPECT_EQ(hash, expected_hash);
        EXPECT_EQ(id, expected_id);
        done = true;
    });

    send_handshake(expected_hash, expected_id);

    run_until([&] {
        return done;
    });
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

// ============================================================================
// SEND TESTS
// ============================================================================


TEST_F(PeerTest, SendsHandshakeSuccessfully){
    connect_peer();

    std::array<char, 20> info_hash{};
    std::array<char, 20> own_id{};

    for (int i = 0; i < 20; ++i) {
        info_hash[i] = static_cast<char>(i);
        own_id[i] = static_cast<char>(20 + i);
    }

    bool done = false;

    peer->send_handshake(info_hash, own_id, [&](asio::error_code ec, Peer&, bool ignored) {
        EXPECT_FALSE(ec);
        EXPECT_FALSE(ignored);
        done = true;
    });

    run_until([&] {
        return done;
    });

    std::array<char, 68> received{};
    bool collected = false;
    asio::async_read(client, asio::buffer(received), [&](asio::error_code ec, size_t){
        EXPECT_FALSE(ec);
        collected = true;
    });

    run_until([&]{
        return collected;
    });

    constexpr std::array<char, 20> protocol{
        19, 'B', 'i', 't', 'T', 'o', 'r', 'r', 'e', 'n',
        't', ' ', 'p', 'r', 'o', 't', 'o', 'c', 'o', 'l'
    };

    EXPECT_TRUE(std::equal(
        protocol.begin(),
        protocol.end(),
        received.begin()
    ));

    for (size_t i = 20; i < 28; ++i)
        EXPECT_EQ(received[i], 0);

    EXPECT_TRUE(std::equal(
        info_hash.begin(),
        info_hash.end(),
        received.begin() + 28
    ));

    EXPECT_TRUE(std::equal(
        own_id.begin(),
        own_id.end(),
        received.begin() + 48
    ));
}


TEST_F(PeerTest, SendsInterested){
    connect_peer();
    do_handshake();

    bool done = false;

    peer->set_interested(true, [&](asio::error_code ec, Peer& p, bool ignored){
        EXPECT_FALSE(ec);
        EXPECT_FALSE(ignored);
        EXPECT_TRUE(p.am_interested());
        done = true;
    });

    run_until([&]{
        return done;
    });

    std::array<char, 5> received{};
    bool collected = false;
    asio::async_read(client, asio::buffer(received), [&](asio::error_code ec, size_t){
        EXPECT_FALSE(ec);
        collected = true;
    });

    run_until([&]{
        return collected;
    });

    EXPECT_EQ(received[0], 0);
    EXPECT_EQ(received[1], 0);
    EXPECT_EQ(received[2], 0);
    EXPECT_EQ(received[3], 1);
    EXPECT_EQ(received[4], std::to_underlying(PeerMessageType::INTERESTED));
}


TEST_F(PeerTest, SendsNotInterested){
    connect_peer();
    do_handshake();

    bool interested_done = false;
    bool not_interested_done = false;

    peer->set_interested(true, [&](asio::error_code ec, Peer&, bool){
        EXPECT_FALSE(ec);
        interested_done = true;
    });

    run_until([&]{
        return interested_done;
    });

    peer->set_interested(false, [&](asio::error_code ec, Peer& p, bool ignored){
        EXPECT_FALSE(ec);
        EXPECT_FALSE(ignored);
        EXPECT_FALSE(p.am_interested());
        not_interested_done = true;
    });

    run_until([&]{
        return not_interested_done;
    });

    std::array<char, 10> received{};
    bool collected = false;
    asio::async_read(client, asio::buffer(received), [&](asio::error_code ec, size_t){
        EXPECT_FALSE(ec);
        collected = true;
    });

    run_until([&]{
        return collected;
    });

    EXPECT_EQ(received[4], std::to_underlying(PeerMessageType::INTERESTED));

    EXPECT_EQ(received[9], std::to_underlying(PeerMessageType::NOT_INTERESTED));
}


TEST_F(PeerTest, SendsUnchoke){
    connect_peer();
    do_handshake();

    bool done = false;

    peer->set_choked(false, [&](asio::error_code ec, Peer& p, bool ignored){
        EXPECT_FALSE(ec);
        EXPECT_FALSE(ignored);
        EXPECT_FALSE(p.is_choked());
        done = true;
    });

    run_until([&]{
        return done;
    });

    std::array<char, 5> received{};
    bool collected = false;
    asio::async_read(client, asio::buffer(received), [&](asio::error_code ec, size_t){
        EXPECT_FALSE(ec);
        collected = true;
    });

    run_until([&]{
        return collected;
    });

    EXPECT_EQ(received[0], 0);
    EXPECT_EQ(received[1], 0);
    EXPECT_EQ(received[2], 0);
    EXPECT_EQ(received[3], 1);
    EXPECT_EQ(received[4], std::to_underlying(PeerMessageType::UNCHOKE));
}


TEST_F(PeerTest, SendsChokeAfterUnchoke){
    connect_peer();
    do_handshake();

    bool unchoke_done = false;
    bool choke_done = false;

    peer->set_choked(false, [&](asio::error_code ec, Peer&, bool){
        EXPECT_FALSE(ec);
        unchoke_done = true;
    });

    run_until([&]{
        return unchoke_done;
    });

    peer->set_choked(true, [&](asio::error_code ec, Peer& p, bool ignored){
        EXPECT_FALSE(ec);
        EXPECT_FALSE(ignored);
        EXPECT_TRUE(p.am_choked());
        choke_done = true;
    });

    run_until([&]{
        return choke_done;
    });

    std::array<char, 10> received{};
    bool collected = false;
    asio::async_read(client, asio::buffer(received), [&](asio::error_code ec, size_t){
        EXPECT_FALSE(ec);
        collected = true;
    });

    run_until([&]{
        return collected;
    });

    EXPECT_EQ(received[4], std::to_underlying(PeerMessageType::UNCHOKE));

    EXPECT_EQ(received[9], std::to_underlying(PeerMessageType::CHOKE));
}


TEST_F(PeerTest, QueuedMessagesAreSentInOrder){
    connect_peer();
    do_handshake();

    bool first_done = false;
    bool second_done = false;

    peer->set_choked(false, [&](asio::error_code ec, Peer&, bool ignored){
        EXPECT_FALSE(ec);
        EXPECT_FALSE(ignored);
        first_done = true;
    });

    peer->set_interested(true, [&](asio::error_code ec, Peer&, bool ignored){
        EXPECT_FALSE(ec);
        EXPECT_FALSE(ignored);
        second_done = true;
    });

    run_until([&]{
        return second_done;
    });

    EXPECT_TRUE(first_done);

    std::array<char, 10> received{};
    bool collected = false;
    asio::async_read(client, asio::buffer(received), [&](asio::error_code ec, size_t){
        EXPECT_FALSE(ec);
        collected = true;
    });

    run_until([&]{
        return collected;
    });

    EXPECT_EQ(received[4], std::to_underlying(PeerMessageType::UNCHOKE));

    EXPECT_EQ(received[9], std::to_underlying(PeerMessageType::INTERESTED));
}


TEST_F(PeerTest, SecondPendingSendIsQueued){
    connect_peer();
    do_handshake();

    bool first_done = false;
    bool second_done = false;

    peer->set_choked(false, [&](asio::error_code ec, Peer&, bool){
        EXPECT_FALSE(ec);
        first_done = true;
    });

    peer->set_choked(true, [&](asio::error_code ec, Peer&, bool){
        EXPECT_FALSE(ec);
        second_done = true;
    });

    run_until([&]{
        return first_done && second_done;
    });

    ASSERT_TRUE(first_done);
    ASSERT_TRUE(second_done);

    std::array<char, 10> received{};
    bool collected = false;
    asio::async_read(client, asio::buffer(received), [&](asio::error_code ec, size_t){
        EXPECT_FALSE(ec);
        collected = true;
    });

    run_until([&]{
        return collected;
    });

    EXPECT_EQ(received[4], std::to_underlying(PeerMessageType::UNCHOKE));

    EXPECT_EQ(received[9], std::to_underlying(PeerMessageType::CHOKE));
}

// ============================================================================
// ERROR TESTS
// ============================================================================

TEST_F(PeerTest, RejectsInvalidHandshake){
    connect_peer();

    std::array<char, 68> buffer{};

    buffer[0] = 19;
    std::memcpy(buffer.data() + 1, "INVALID protocol", 16);

    asio::write(client, asio::buffer(buffer));

    bool done = false;
    asio::error_code result;

    peer->read_handshake([&](asio::error_code ec, const auto&, const auto&, Peer&, bool ignored){
        EXPECT_FALSE(ignored);
        result = ec;
        done = true;
    });

    run_until([&] {
        return done;
    });

    EXPECT_EQ(result, asio::error::operation_not_supported);
}

TEST_F(PeerTest, RejectsReadBeforeHandshake){
    connect_peer();

    bool done = false;

    peer->read_message([&](asio::error_code ec, PeerMessage, Peer&, bool ignored){
        EXPECT_FALSE(ec);
        EXPECT_TRUE(ignored);
        done = true;
    });

    run_until([&] { return done; });
    EXPECT_TRUE(done);
}

TEST_F(PeerTest, RejectsSendingBeforeHandshake){
    connect_peer();

    bool interested_done = false;
    bool unchoke_done = false;

    peer->set_interested(true, [&](asio::error_code ec, Peer&, bool ignored){
        EXPECT_FALSE(ec);
        EXPECT_TRUE(ignored);
        interested_done = true;
    });

    peer->set_choked(false, [&](asio::error_code ec, Peer&, bool ignored){
        EXPECT_FALSE(ec);
        EXPECT_TRUE(ignored);
        unchoke_done = true;
    });

    run_until([&]{
        return interested_done && unchoke_done;
    });

    ASSERT_TRUE(interested_done);
    ASSERT_TRUE(unchoke_done);
    EXPECT_EQ(client.available(), 0U);
}

TEST_F(PeerTest, SendingHandshakeDoesNotUnlockReceiving){
    connect_peer();
    receive_handshake_from_peer();

    bool done = false;

    peer->read_message([&](asio::error_code ec, PeerMessage, Peer&, bool ignored){
        EXPECT_FALSE(ec);
        EXPECT_TRUE(ignored);
        done = true;
    });

    run_until([&] { return done; });
}

TEST_F(PeerTest, ReceivingHandshakeDoesNotUnlockSending){
    connect_peer();
    send_handshake({}, {});
    bool handshake_received = false;
    peer->read_handshake([&](asio::error_code ec, const Hash20&, const PeerId&, Peer&, bool ignored){
        EXPECT_FALSE(ec);
        EXPECT_FALSE(ignored);
        handshake_received = true;
    });
    run_until([&] { return handshake_received; });

    bool done = false;
    peer->set_interested(
        true,
        [&](asio::error_code ec, Peer&, bool ignored) {
            EXPECT_FALSE(ec);
            EXPECT_TRUE(ignored);
            done = true;
        });

    run_until([&] { return done; });
    EXPECT_EQ(client.available(), 0U);
}

TEST_F(PeerTest, RejectsSecondPendingRead){
    connect_peer();
    do_handshake();

    bool first_done = false;
    bool second_done = false;

    peer->read_message([&](asio::error_code ec, PeerMessage, Peer&, bool ignored){
        EXPECT_FALSE(ignored);
        EXPECT_FALSE(ec);
        first_done = true;
    });

    peer->read_message([&](asio::error_code ec, PeerMessage, Peer&, bool ignored){
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

    // id = 9, wrong value
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

TEST_F(PeerTest, DisconnectInSendHandler){
    connect_peer();
    receive_handshake_from_peer();

    bool next_done = false;

    peer->set_interested(true, [&](asio::error_code ec, Peer& p, bool ignored) {
        ASSERT_FALSE(ec);
        ASSERT_FALSE(ignored);

        p.disconnect();

        p.set_choked(false, [&](asio::error_code next_ec, Peer&, bool) {
            EXPECT_EQ(next_ec, asio::error::not_connected);
            next_done = true;
        });
    });

    run_until([&] { return next_done; });
    EXPECT_TRUE(next_done);
}

TEST_F(PeerTest, DisconnectInReadHandler){
    connect_peer();
    do_handshake();

    bool next_done = false;

    peer->read_message([&](asio::error_code ec, PeerMessage msg, Peer& p, bool ignored) {
        ASSERT_FALSE(ec);
        ASSERT_FALSE(ignored);
        ASSERT_EQ(msg.type, PeerMessageType::UNCHOKE);

        p.disconnect();

        p.read_message([&](asio::error_code next_ec, PeerMessage, Peer&, bool) {
            EXPECT_EQ(next_ec, asio::error::not_connected);
            next_done = true;
        });
    });

    send_message(PeerMessageType::UNCHOKE);

    run_until([&] { return next_done; });
    EXPECT_TRUE(next_done);
}

} // namespace
} // namespace torrent
