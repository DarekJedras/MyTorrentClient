#include <gtest/gtest.h>
#include <httplib.h>
#include <thread>
#include <chrono>
#include <string>
#include "tracker.hpp"

namespace torrent{
namespace {

using namespace std::string_literals;

class TrackerIntegrationTest : public ::testing::Test {
protected:
    httplib::Server svr;
    std::thread server_thread;
    const std::string host = "127.0.0.1";
    const int port = 18080;

    void SetUp() override {
        server_thread = std::thread([this]() {
            svr.listen(host, port);
        });

        while (!svr.is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void TearDown() override {
        svr.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }
};

// ============================================================================
// Integration tests
// ============================================================================

TEST_F(TrackerIntegrationTest, SuccessfulAnnounceParsesPeersCorrectly) {
    TrackerRequest request = {
        .info_hash{"SomeHashValue"},
        .peer_id{"UniquePeerId"},
        .uploaded{0},
        .downloaded{0},
        .left{0x1122334455667788},
        .event{TrackerEvent::Started},
        .key{0x12345678},
        .port{0x4321}
    };

    std::string tracker_url = "http://" + host + ":" + std::to_string(port) + "/announce";
    
    Tracker tracker(tracker_url);

    std::string bencoded_response = 
        "d8:intervali1800e5:peers12:"s + 
        "\xC0\xA8\x01\x01\x1F\x90"s + // 192.168.1.1:8080
        "\x0A\x00\x00\x2A\x1A\x10"s + // 10.0.0.42:6672
        "e"s;

    svr.Get("/announce", [bencoded_response](const httplib::Request& req, httplib::Response& res) {
        EXPECT_TRUE(req.has_param("info_hash") && req.get_param_value("info_hash") == "SomeHashValue\0\0\0\0\0\0\0"s);
        EXPECT_TRUE(req.has_param("peer_id") && req.get_param_value("peer_id") == "UniquePeerId\0\0\0\0\0\0\0\0"s);
        EXPECT_TRUE(req.has_param("port") && req.get_param_value("port") == std::to_string(0x4321));
        EXPECT_TRUE(req.has_param("uploaded") && req.get_param_value("uploaded") == std::to_string(0));
        EXPECT_TRUE(req.has_param("downloaded") && req.get_param_value("downloaded") == std::to_string(0));
        EXPECT_TRUE(req.has_param("left") && req.get_param_value("left") == std::to_string(0x1122334455667788));
        EXPECT_TRUE(req.has_param("key") && req.get_param_value("key") == std::to_string(0x12345678));
        EXPECT_TRUE(req.has_param("event") && req.get_param_value("event") == to_string(TrackerEvent::Started));

        res.set_content(bencoded_response, "text/plain");
        res.status = 200;
    });

    auto response = tracker.announce(request);

    EXPECT_EQ(response.interval, 1800);
    ASSERT_EQ(response.peers.size(), 2);
    
    EXPECT_EQ(response.peers[0].ip, "192.168.1.1");
    EXPECT_EQ(response.peers[0].port, 8080);
    
    EXPECT_EQ(response.peers[1].ip, "10.0.0.42");
    EXPECT_EQ(response.peers[1].port, 6672);
}

TEST_F(TrackerIntegrationTest, HandlesTrackerErrorResponse) {

    std::string failure_response = "d14:failure reason17:invalid info-hashe";

    svr.Get("/announce", [failure_response](const httplib::Request&, httplib::Response& res) {
        res.set_content(failure_response, "text/plain");
        res.status = 200;
    });

    std::string tracker_url = "http://" + host + ":" + std::to_string(port) + "/announce";
    Tracker tracker(tracker_url);

    TrackerRequest request = {
        .info_hash{"invalidHashValue"},
        .peer_id{"UniquePeerId"},
        .uploaded{0},
        .downloaded{0},
        .left{0x1122334455667788},
        .event{TrackerEvent::Started},
        .key{0x12345678},
        .port{0x4321}
    };

    TrackerResponse response = tracker.announce(request);

    ASSERT_TRUE(response.failure_reason.has_value());
    EXPECT_EQ(response.failure_reason, "invalid info-hash");
}

TEST_F(TrackerIntegrationTest, HandlesHttp404NotFound) {
    std::string tracker_url = "http://" + host + ":" + std::to_string(port) + "/invalid_path";
    Tracker tracker(tracker_url);
    TrackerRequest request = {
        .info_hash{"invalidHashValue"},
        .peer_id{"UniquePeerId"},
        .uploaded{0},
        .downloaded{0},
        .left{0x1122334455667788},
        .event{TrackerEvent::Started},
        .key{0x12345678},
        .port{0x4321}
    };
    EXPECT_THROW(tracker.announce(request), std::runtime_error);
}

} // namespace
} // namespace torrent
