#include "piece_manager.hpp"
#include "torrent_file.hpp"
#include "tracker.hpp"
#include "peer.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>

namespace torrent{

class PeerStatistics {
    using Clock = std::chrono::steady_clock;

    uint64_t _download_rate{0};
    uint64_t _upload_rate{0};

    uint64_t _uploaded_at_last_calc;
    uint64_t _downloaded_at_last_calc;

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
    struct PeerSession {
        Peer connection;
        PeerStatistics stats;
        PeerInfo info;
        std::vector<bool> pieces_bitmask;
    };
    std::vector<PeerSession> peers;
    PieceManager piece_manager;
    TorrentFile torrent_info;
    Tracker tracker;
};

} // namespace torrent
