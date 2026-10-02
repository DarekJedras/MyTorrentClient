#include <gtest/gtest.h>
#include "tracker.hpp"

namespace torrent {
namespace {

using namespace std::string_literals;

// ============================================================================
// TrackerEvent to_string conversion
// ============================================================================
TEST(TrackerEventTest, ConvertsEventsToStringCorrectly) {
    EXPECT_EQ(to_string(TrackerEvent::Started), "started");
    EXPECT_EQ(to_string(TrackerEvent::Stopped), "stopped");
    EXPECT_EQ(to_string(TrackerEvent::Completed), "completed");
    EXPECT_EQ(to_string(TrackerEvent::None), "empty");
}

// ============================================================================
// URL Parsing in Tracker class
// ============================================================================

TEST(TrackerConstructorTest, ParsesStandardHttpUrlWithPort) {
    Tracker tracker("http://tracker.example.com:6969/announce");
    EXPECT_EQ(tracker.url(), "http://tracker.example.com:6969/announce");
}

TEST(TrackerConstructorTest, DeducesDefaultPortForHttp) {
    Tracker tracker("http://tracker.example.com/announce");
    EXPECT_EQ(tracker.url(), "http://tracker.example.com:80/announce");
}

TEST(TrackerConstructorTest, DeducesDefaultPortForHttps) {
    Tracker tracker("https://tracker.example.com/announce");
    EXPECT_EQ(tracker.url(), "https://tracker.example.com:443/announce");
}

TEST(TrackerConstructorTest, HandlesUrlWithQueryParams) {
    Tracker tracker("http://tracker.example.com:8080/announce?passkey=12345");
    EXPECT_EQ(tracker.url(), "http://tracker.example.com:8080/announce");
}

TEST(TrackerConstructorTest, HandlesMissingPathBySettingRoot) {
    Tracker tracker("http://tracker.example.com:8080");
    EXPECT_EQ(tracker.url(), "http://tracker.example.com:8080/");
}

// ============================================================================
// Invalid URLs
// ============================================================================

TEST(TrackerConstructorTest, ThrowsOnMissingProtocol) {
    EXPECT_THROW(Tracker("tracker.example.com:6969/announce"), std::runtime_error);
}

TEST(TrackerConstructorTest, ThrowsOnInvalidHostDelimiter) {
    EXPECT_THROW(Tracker("http:/tracker.example.com/announce"), std::runtime_error);
    EXPECT_THROW(Tracker("http:tracker.example.com/announce"), std::runtime_error);
}

TEST(TrackerConstructorTest, ThrowsOnInvalidPortNumber) {
    EXPECT_THROW(Tracker("http://tracker.example.com:abc/announce"), std::runtime_error);
    EXPECT_THROW(Tracker("http://tracker.example.com:70000/announce"), std::runtime_error);
}

TEST(TrackerConstructorTest, ThrowsWhenCannotDeducePortForUnknownProtocol) {
    EXPECT_THROW(Tracker("udp://tracker.example.com/announce"), std::runtime_error);
}

// ============================================================================
// Static Method Tracker::parse_peers_list (Compact Format)
// ============================================================================

TEST(TrackerPeersTest, ParsesEmptyPeersBinary) {
    auto peers = Tracker::parse_peers_list("");
    EXPECT_TRUE(peers.empty());
}

TEST(TrackerPeersTest, IPv4CompactMultiplePeers1) {
    // Peer 1: 192.168.1.1:8080   (192.168.1.1 = \xC0\xA8\x01\x01, 8080 = 0x1F90 = \x1F\x90)
    // Peer 2: 10.0.0.42:6672     (10.0.0.42  = \x0A\x00\x00\x2A, 6672 = 0x1A10 = \x1A\x10)
    std::string compact_data = 
        "\xC0\xA8\x01\x01\x1F\x90"
        "\x0A\x00\x00\x2A\x1A\x10"s;

    BencodeValue value = BencodeString(compact_data);
    auto peers = Tracker::parse_peers_list(value);

    ASSERT_EQ(peers.size(), 2);

    EXPECT_EQ(peers[0].ip, "192.168.1.1");
    EXPECT_EQ(peers[0].port, 8080);

    EXPECT_EQ(peers[1].ip, "10.0.0.42");
    EXPECT_EQ(peers[1].port, 6672);
}

TEST(TrackerPeersTest, IPv4CompactMultiplePeers2) {
    std::string compact_data = 
        "\x86\xC8\x00\x01\x7D\x50"
        "\x7F\x00\x00\x01\x1A\xE1"s;

    BencodeValue value = BencodeString(compact_data);
    auto peers = Tracker::parse_peers_list(value);

    ASSERT_EQ(peers.size(), 2);
    EXPECT_EQ(peers[0].ip, "134.200.0.1");
    EXPECT_EQ(peers[0].port, 32080);
    EXPECT_EQ(peers[1].ip, "127.0.0.1");
    EXPECT_EQ(peers[1].port, 6881);
}

TEST(TrackerPeersTest, IPv6CompactSinglePeerLoopback) {
    std::string compact6_data(
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01"
        "\x1A\xE1", 
        18
    );

    BencodeValue value = BencodeString(compact6_data);
    auto peers = Tracker::parse_peers_list(value, /*is_peers6=*/true);

    ASSERT_EQ(peers.size(), 1);
    EXPECT_EQ(peers[0].ip, "::1");
    EXPECT_EQ(peers[0].port, 6881);
}

TEST(TrackerPeersTest, IPv6CompactMultiplePeers) {
    // Peer 1: 2001:db8::1:1 (2001:0db8:0000:0000:0000:0000:0001:0001) : 8080 (0x1F90)
    // Peer 2: fe80::1               (fe80:0000:0000:0000:0000:0000:0000:0001) : 51413 (0xC8D5)
    
    const uint8_t raw_data[] = {
        // Peer 1 IP
        0x20, 0x01, 0x0D, 0xB8, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
        // Peer 1 Port
        0x1F, 0x90,

        // Peer 2 IP
        0xFE, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        // Peer 2 Port
        0xC8, 0xD5
    };

    std::string compact6_data(reinterpret_cast<const char*>(raw_data), sizeof(raw_data));

    BencodeValue value = BencodeString(compact6_data);
    auto peers = Tracker::parse_peers_list(value, /*is_peers6=*/true);

    ASSERT_EQ(peers.size(), 2);
    
    EXPECT_EQ(peers[0].ip, "2001:db8::1:1");
    EXPECT_EQ(peers[0].port, 8080);

    EXPECT_EQ(peers[1].ip, "fe80::1");
    EXPECT_EQ(peers[1].port, 51413);
}

TEST(TrackerPeersTest, IPv6CompactAllZerosAddress) {
    std::string compact6_data(18, '\0');

    BencodeValue value = BencodeString(compact6_data);
    auto peers = Tracker::parse_peers_list(value, /*is_peers6=*/true);

    ASSERT_EQ(peers.size(), 1);
    EXPECT_EQ(peers[0].ip, "::");
    EXPECT_EQ(peers[0].port, 0);
}

// ============================================================================
// Static Method Tracker::parse_peers_list (Original Dictionary List Format)
// ============================================================================

TEST(TrackerPeersTest, IPv4BencodeListFormat1) {
    BencodeDict peer1{
        {"ip", BencodeString("127.0.0.1")},
        {"port", BencodeInt(6881)},
        {"peer id", BencodeString("-UT3530-123456789012")}
    };

    BencodeDict peer2{
        {"ip", BencodeString("8.8.8.8")},
        {"port", BencodeInt(80)},
        {"peer id", BencodeString("-TR2940-987654321098")}
    };

    BencodeList peer_list{peer1, peer2};
    BencodeValue value = peer_list;

    auto peers = Tracker::parse_peers_list(value);

    ASSERT_EQ(peers.size(), 2);

    EXPECT_EQ(peers[0].ip, "127.0.0.1");
    EXPECT_EQ(peers[0].port, 6881);
    EXPECT_EQ(std::string(peers[0].peer_id.value().data(), 20), "-UT3530-123456789012"s);

    EXPECT_EQ(peers[1].ip, "8.8.8.8");
    EXPECT_EQ(peers[1].port, 80);
    EXPECT_EQ(std::string(peers[1].peer_id.value().data(), 20), "-TR2940-987654321098"s);
}

TEST(TrackerPeersTest, IPv4BencodeListFormat2) {
    BencodeDict peer1{
        {"ip", BencodeString("10.0.0.1")},
        {"port", static_cast<BencodeInt>(80)},
        {"peer id", BencodeString("-TR\r\n \t987654Bde\x00""098"s)}
    };
    BencodeDict peer2{
        {"ip", BencodeString("172.16.0.1")},
        {"port", static_cast<BencodeInt>(443)},
        {"peer id", BencodeString("-TRaa0-987654Bde\x00""098"s)}
    };
    
    BencodeList list{peer1, peer2};
    BencodeValue value = list;

    auto peers = Tracker::parse_peers_list(value, /*is_peers6=*/false);

    ASSERT_EQ(peers.size(), 2);
    EXPECT_EQ(peers[0].ip, "10.0.0.1");
    EXPECT_EQ(peers[0].port, 80);
    ASSERT_TRUE(peers[0].peer_id.has_value());
    EXPECT_EQ(std::string(peers[0].peer_id.value().data(), 20), "-TR\r\n \t987654Bde\x00""098"s);
    EXPECT_EQ(peers[1].ip, "172.16.0.1");
    EXPECT_EQ(peers[1].port, 443);
    ASSERT_TRUE(peers[1].peer_id.has_value());
    EXPECT_EQ(std::string(peers[1].peer_id.value().data(), 20), "-TRaa0-987654Bde\x00""098"s);
}

TEST(TrackerPeersTest, IPv6BencodeListFormat) {
    BencodeDict peer1{
        {"ip", BencodeString("2001:db8::8a2e:370:7334")},
        {"port", static_cast<BencodeInt>(6881)},
        {"peer id", BencodeString("-12345\x14""987654Bde\x00""098"s)}
    };
    
    BencodeList list{peer1};
    BencodeValue value = list;

    auto peers = Tracker::parse_peers_list(value, /*is_peers6=*/true);

    ASSERT_EQ(peers.size(), 1);
    EXPECT_EQ(peers[0].ip, "2001:db8::8a2e:370:7334");
    EXPECT_EQ(peers[0].port, 6881);
}

// ============================================================================
// Static Method Tracker::parse_peers_list (Invalid Formats / Edge Cases)
// ============================================================================

TEST(TrackerPeersTest, HandlesEmptyPeersList) {
    BencodeValue empty_string = BencodeString("");
    EXPECT_TRUE(Tracker::parse_peers_list(empty_string).empty());
    EXPECT_TRUE(Tracker::parse_peers_list(empty_string, true).empty());

    BencodeValue empty_list = BencodeList{};
    EXPECT_TRUE(Tracker::parse_peers_list(empty_list).empty());
}

TEST(TrackerPeersTest, ThrowsOnInvalidValueType) {
    BencodeValue invalid_value = BencodeInt(12345);
    EXPECT_THROW(Tracker::parse_peers_list(invalid_value), std::runtime_error);
}

TEST(TrackerPeersTest, ThrowsOnMalformedDictionaryInList) {
    BencodeList malformed_list{
        BencodeInt(100),
        BencodeString("invalid_peer")
    };
    BencodeValue value = malformed_list;

    EXPECT_THROW(Tracker::parse_peers_list(value), std::runtime_error);
}

TEST(TrackerPeersTest, ThrowsOnInvalidCompactLength) {
    std::string invalid_compact_data = "\xC0\xA8\x01\x01\x1F\x90\xFF";
    BencodeValue value = BencodeString(invalid_compact_data);

    EXPECT_THROW(Tracker::parse_peers_list(value), std::runtime_error);
    EXPECT_THROW(Tracker::parse_peers_list(value, true), std::runtime_error);
}

} // namespace
} // namespace torrent
