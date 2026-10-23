#include "download_engine.hpp"
#include <chrono>

namespace torrent {

void PeerStatistics::update_rates(){
    const auto now = Clock::now();
    auto uploaded_now = _uploaded_total.load(std::memory_order_relaxed);

    auto downloaded_now = _downloaded_total.load(std::memory_order_relaxed);

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - _last_rate_calc
        ).count();

    if (elapsed_ms <= 0)
        return;

    _upload_rate = (uploaded_now - _uploaded_at_last_calc) / elapsed_ms;

    _download_rate = (downloaded_now - _downloaded_at_last_calc) / elapsed_ms;

    _uploaded_at_last_calc = uploaded_now;
    _downloaded_at_last_calc = downloaded_now;
}

}
