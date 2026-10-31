#ifndef DOWNLOAD_ENGINE_HPP
#define DOWNLOAD_ENGINE_HPP

#include "piece_manager.hpp"
#include "torrent_file.hpp"
#include "tracker.hpp"
#include "peer.hpp"
#include "asio.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <queue>
#include <variant>
#include <condition_variable>
#include <mutex>

namespace torrent{

class PeerStatistics {
    using Clock = std::chrono::steady_clock;

    uint64_t _download_rate{0};
    uint64_t _upload_rate{0};

    uint64_t _uploaded_at_last_calc{0};
    uint64_t _downloaded_at_last_calc{0};

    std::atomic<uint64_t> _uploaded_total{0};
    std::atomic<uint64_t> _downloaded_total{0};

    Clock::time_point _last_rate_calc{Clock::now()};

public:
    uint64_t downloaded() const {
        return _downloaded_total.load(std::memory_order_relaxed);
    }

    uint64_t uploaded() const {
        return _uploaded_total.load(std::memory_order_relaxed);
    }

    uint64_t download_rate() const { return _download_rate; }
    uint64_t upload_rate() const { return _upload_rate; }

    void update_uploaded(uint64_t n) {
        _uploaded_total.fetch_add(n, std::memory_order_relaxed);
    }

    void update_downloaded(uint64_t n) {
        _downloaded_total.fetch_add(n, std::memory_order_relaxed);
    }

    void update_rates();
};

class Engine {
public:
    static constexpr size_t MAX_BLOCK_LENGTH = 1U << 14; // 16 kiB

    using asio_tcp = asio::ip::tcp;
    using time_point = std::chrono::time_point<std::chrono::steady_clock>;
    struct PeerSession {
        std::shared_ptr<Peer> connection;
        std::shared_ptr<PeerStatistics> stats;
        PeerInfo info;
        std::vector<bool> pieces_bitmask;
        std::atomic<bool> is_alive = true;

        PeerSession(std::shared_ptr<Peer> connection_, PeerInfo info_, size_t piece_count)
            : connection(std::move(connection_)),
              stats(std::make_shared<PeerStatistics>()),
              info(std::move(info_)),
              pieces_bitmask(piece_count, false) {}
    };

    struct PeerConnected {
        std::shared_ptr<PeerSession> session; // needs shared_ptr because session isn't emplaced yet
    };

    struct PeerDisconnected {
        std::weak_ptr<PeerSession> session;
        asio::error_code reason;
    };

    struct PeerHandshakeCompleted{
        std::weak_ptr<PeerSession> session;
    };

    struct PeerBitmaskUpdate{
        std::weak_ptr<PeerSession> session;
        // size_t means an index of the newly acquired piece
        // std::vector is a complete bitmask in raw form
        std::variant<std::vector<char>, size_t> pieces;
    };

    struct PeerChangedState {
        std::weak_ptr<PeerSession> session;
        PeerMessageType msg_type;
    };

    struct PieceBlockReceived {
        std::weak_ptr<PeerSession> session;
        size_t piece_idx;
        size_t offset;
        std::vector<char> data;
    };

    struct PieceBlockRequested {
        std::weak_ptr<PeerSession> session;
        size_t piece_idx;
        size_t offset;
        size_t length;
        bool is_cancel;
    };

    struct PieceDownloadExpected {
        std::weak_ptr<PeerSession> session;
    };

    using Event = std::variant<
        PeerConnected,
        PeerHandshakeCompleted,
        PeerDisconnected,
        PeerChangedState,
        PeerBitmaskUpdate,
        PieceBlockReceived,
        PieceBlockRequested,
        PieceDownloadExpected
    >;

    std::queue<Event> _events;
    std::mutex _events_mutex;
    std::condition_variable _events_cv;

    void enqueue_event(Event event);

    void request_peer_disconnect(std::weak_ptr<PeerSession> weak_session, asio::error_code ec);
    void enqueue_received_messages(std::shared_ptr<PeerSession> session);

private:
    std::atomic<bool> _stopping = false;

    std::vector<std::shared_ptr<PeerSession>> _peers;
    std::vector<PeerInfo> _peer_candidates;
    TorrentFile _torrent_info;
    PieceManager _piece_manager;
    Tracker _tracker;

    asio::io_context _io;
    asio_tcp::acceptor _acceptor;

    PeerId _own_id;
    uint32_t _download_key;

    // stats kept for tracker announces
    uint64_t _total_uploaded{0};
    uint64_t _total_downloaded{0};
    // until resuming download is implemented it's initial value is total torrent length
    uint64_t _left_to_download;

    time_point announce_to_tracker(TrackerEvent event);

    void reach_peer(const PeerInfo& info);
    void do_accept();

    void handle_event(Event event);
    void handle_event_impl(PeerConnected event);
    void handle_event_impl(PeerDisconnected event);
    void handle_event_impl(PeerHandshakeCompleted event);
    void handle_event_impl(PeerChangedState event);
    void handle_event_impl(PeerBitmaskUpdate event);
    void handle_event_impl(PieceBlockReceived event);
    void handle_event_impl(PieceBlockRequested event);
    void handle_event_impl(PieceDownloadExpected event);

    void schedule_requests();
    void update_choking();
    void update_rates();

    bool is_download_complete() const;

public:
    Engine(TorrentFile torrent, const std::filesystem::path& temp_file_path);

    void run();
};

} // namespace torrent

#endif
