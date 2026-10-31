#include "download_engine.hpp"
#include "tracker.hpp"
#include "utils.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <optional>

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

Engine::Engine(TorrentFile torrent, const std::filesystem::path& temp_file_path)
    : _torrent_info(std::move(torrent)),
      _piece_manager(_torrent_info, DiskStorage(temp_file_path)),
      _tracker(_torrent_info.announce()),
      _acceptor(_io),
      _download_key(utils::get_random_bytes<uint32_t>()),
      _left_to_download(_torrent_info.total_file_length())
    {
        _acceptor.open(asio::ip::tcp::v4());
        for (uint16_t i = 6881; i<6890; i++){
            asio::error_code ec;
            _acceptor.bind(asio_tcp::endpoint(asio_tcp::v4(), i), ec);
            if (!ec) break;
        }
        char* id_ptr = _own_id.data();
        for (int i = 0; i<5; i++) utils::write_net_buffer(id_ptr, utils::get_random_bytes<uint32_t>());
}

void Engine::enqueue_event(Event event){
    {
        std::lock_guard lock(_events_mutex);
        _events.push(std::move(event));
    }
    _events_cv.notify_one();
}

void Engine::enqueue_received_messages(std::shared_ptr<PeerSession> session){
    session->connection->read_message(
        [this, session](asio::error_code ec, PeerMessage message, Peer&, bool ignored){
            if (ec){
                request_peer_disconnect(session, ec);
                return;
            }
            if (ignored || !session->is_alive.load(std::memory_order_relaxed)) return;

            switch (message.type){
                case PeerMessageType::UNCHOKE:
                case PeerMessageType::CHOKE:
                case PeerMessageType::NOT_INTERESTED:
                case PeerMessageType::INTERESTED:
                    enqueue_event(PeerChangedState{session, message.type});
                    break;

                case PeerMessageType::BITFIELD: {
                    std::vector<char> bitmask{message.payload.begin(), message.payload.end()};
                    enqueue_event(PeerBitmaskUpdate{session, std::move(bitmask)});
                    break;
                }

                case PeerMessageType::HAVE: {
                    if (message.payload.size() != 12) break;

                    const char* data_ptr = message.payload.data();
                    size_t idx = utils::read_net_buffer<uint32_t>(data_ptr);
                    enqueue_event(PeerBitmaskUpdate{session, idx});
                    break;
                }

                case PeerMessageType::REQUEST: {
                    if (message.payload.size() != 12) break;

                    const char* data_ptr = message.payload.data();
                    size_t idx = utils::read_net_buffer<uint32_t>(data_ptr);
                    size_t offset = utils::read_net_buffer<uint32_t>(data_ptr);
                    size_t length = utils::read_net_buffer<uint32_t>(data_ptr);
                    if (length == 0 || length > MAX_BLOCK_LENGTH) break;

                    enqueue_event(PieceBlockRequested{session, idx, offset, length, false});
                    break;
                }

                case PeerMessageType::CANCEL: {
                    if (message.payload.size() != 12) break;

                    const char* data_ptr = message.payload.data();
                    size_t idx = utils::read_net_buffer<uint32_t>(data_ptr);
                    size_t offset = utils::read_net_buffer<uint32_t>(data_ptr);
                    size_t length = utils::read_net_buffer<uint32_t>(data_ptr);
                    if (length == 0 || length > MAX_BLOCK_LENGTH) break;

                    enqueue_event(PieceBlockRequested{session, idx, offset, length, true});
                    break;
                }

                case PeerMessageType::PIECE: {
                    if (message.payload.size() < 8) break;

                    const char* data_ptr = message.payload.data();
                    size_t idx = utils::read_net_buffer<uint32_t>(data_ptr);
                    size_t offset = utils::read_net_buffer<uint32_t>(data_ptr);
                    std::vector<char> piece_data{message.payload.begin()+8, message.payload.end()};

                    session->stats->update_downloaded(piece_data.size());

                    enqueue_event(PieceBlockReceived{session, idx, offset, std::move(piece_data)});
                }

                default:
                    break;
            }
            enqueue_received_messages(session);
        }
    );
}

Engine::time_point Engine::announce_to_tracker(TrackerEvent event){
    TrackerRequest request = {
        .info_hash = _torrent_info.info_hash(),
        .peer_id = _own_id,
        .uploaded = _total_uploaded,
        .downloaded = _total_downloaded,
        .left = _left_to_download,
        .event = event,
        .key = _download_key,
        .port = _acceptor.local_endpoint().port()
    };
    TrackerResponse response = _tracker.announce(request);
    if (response.failure_reason.has_value()){
        std::cerr << "tracker announce failed. failure reason: " << response.failure_reason.value() << '\n';
        if (_peer_candidates.empty() && _peers.empty()) throw std::runtime_error("can't download without peers list");
    }
    if (response.interval < 0) throw std::runtime_error("invalid \"interval\" value received");
    for (auto& peer_info : response.peers){
        _peer_candidates.push_back(peer_info);
    }
    return std::chrono::steady_clock::now() + std::chrono::seconds(response.interval);
}

void Engine::request_peer_disconnect(
    std::weak_ptr<PeerSession> weak_session,
    asio::error_code ec
){
    auto session = weak_session.lock();
    if (!session)
        return;

    if (!session->is_alive.exchange(false, std::memory_order_relaxed))
        return;

    session->connection->disconnect();
    enqueue_event(PeerDisconnected{std::move(session), ec});
}

void Engine::reach_peer(const PeerInfo& info){
    asio::error_code ec;
    asio::ip::address ip_addr = asio::ip::make_address(info.ip, ec);
    if (ec) return;

    auto socket_ptr = std::make_shared<asio_tcp::socket>(asio::make_strand(_io));
    size_t piece_count = _torrent_info.pieces().length()/20;
    socket_ptr->async_connect(
        asio_tcp::endpoint(ip_addr, info.port),
        [this, socket_ptr, info, piece_count](asio::error_code ec){
            if (ec) return;

            auto session = std::make_shared<PeerSession>(
                std::make_shared<Peer>(std::move(*socket_ptr)),
                std::move(info),
                piece_count
            );
            enqueue_event(PeerConnected(std::move(session)));
        }
    );
}

void Engine::do_accept(){
    if (_stopping)
        return;

    auto socket = std::make_shared<asio_tcp::socket>(
        asio::make_strand(_io)
    );

    _acceptor.async_accept(
        *socket,
        [this, socket](asio::error_code ec) {
            if (ec) {
                if (!_stopping.load()) {
                    std::cerr << "Acceptor encountered error: "
                              << ec << '\n';
                    // should be determined based on error type
                    // do_accept();
                }
                return;
            }

            if (_stopping.load())
                return;

            asio::error_code endpoint_ec;
            auto endpoint = socket->remote_endpoint(endpoint_ec);

            if (endpoint_ec) {
                do_accept();
                return;
            }

            const size_t piece_count =
                _torrent_info.pieces().size() / 20;

            auto session = std::make_shared<PeerSession>(
                std::make_shared<Peer>(std::move(*socket)),
                PeerInfo{
                    .ip = endpoint.address().to_string(),
                    .peer_id = std::optional<PeerId>(),
                    .port = endpoint.port()
                },
                piece_count
            );

            enqueue_event(PeerConnected{std::move(session)});

            do_accept();
        }
    );
}

void Engine::run(){
    do_accept();
    time_point next_announce = announce_to_tracker(TrackerEvent::Started);
    while (!_stopping){
        std::optional<Event> event;
        {
            std::unique_lock lock(_events_mutex);
            _events_cv.wait_until(lock, next_announce, [this](){
                return _stopping || !_events.empty();
            });
            
            if (!_events.empty()){
                event = std::move(_events.front());
                _events.pop();
            }
        }
        if (event) handle_event(std::move(*event));

        if (std::chrono::steady_clock::now() >= next_announce){
            next_announce = announce_to_tracker(TrackerEvent::None);
        }
    }
    std::cerr << "Stopping download engine\n";
}

void Engine::handle_event(Event event){
    std::visit([this](auto&& e){
        handle_event_impl(
            std::forward<decltype(e)>(e)
        );
    }, std::move(event));
}

struct HandshakeProgress{
    bool send_done = false;
    bool read_done = false;
    bool disconnect_requested = false;
    bool completion_reported = false;
    std::optional<asio::error_code> fail_reason;
};

void Engine::handle_event_impl(Engine::PeerConnected event){
    auto& new_session = _peers.emplace_back(std::move(event.session));
    auto progress = std::make_shared<HandshakeProgress>();
    auto try_finish = [this, new_session, progress](){
        if (progress->fail_reason && !progress->disconnect_requested){
            progress->disconnect_requested = true;
            request_peer_disconnect(new_session, progress->fail_reason.value());
        }

        if (!progress->read_done || !progress->send_done) return;

        if (progress->completion_reported) return;

        progress->completion_reported = true;
        enqueue_event(PeerHandshakeCompleted(new_session));
    };

    new_session->connection->send_handshake(
        _torrent_info.info_hash(),
        _own_id,
        [this, new_session, progress, try_finish](asio::error_code ec, Peer& peer, bool ignored){
            if ((ec || ignored) && !progress->fail_reason){
                progress->fail_reason = ec ? ec : asio::error::fault;
            }
            progress->send_done = true;
            try_finish();
        }
    );

    new_session->connection->read_handshake(
        [this, new_session, progress, try_finish](
            asio::error_code ec,
            const Hash20& info_hash_recvd,
            const PeerId& peer_id_recvd,
            Peer& peer,
            bool ignored
        ) {
            if ((ec || ignored) && !progress->fail_reason){
                progress->fail_reason = ec ? ec : asio::error::fault;
            }
            if (auto expected_id = new_session->info.peer_id;
                expected_id && expected_id != peer_id_recvd && !progress->fail_reason){
                progress->fail_reason = asio::error::fault;
            }
            if (auto expected_hash = _torrent_info.info_hash();
                expected_hash != info_hash_recvd && !progress->fail_reason){
                progress->fail_reason = asio::error::fault;
            }
            progress->read_done = true;
            try_finish();
        }
    );
}

void Engine::handle_event_impl(Engine::PeerDisconnected event){
    auto session = event.session.lock();
    if (!session) return; // session already deleted

    auto it = std::find(_peers.begin(), _peers.end(), session);
    if (it == _peers.end()) return; // session already deleted

    (*it)->is_alive.store(false, std::memory_order_relaxed);
    _peers.erase(it);
}

void Engine::handle_event_impl(Engine::PeerHandshakeCompleted event){
    auto session = event.session.lock();
    if (!session) {
        return;
    }
    enqueue_received_messages(session);

    size_t pieces_count = _piece_manager.pieces_bitmask().size();
    std::vector<char> payload((pieces_count + 7) / 8, '\0');
    for (size_t i = 0; i < pieces_count; ++i) {
        if (!_piece_manager.pieces_bitmask()[i])
            continue;

        auto byte = static_cast<unsigned char>(payload[i / 8]);
        byte |= static_cast<unsigned char>(0x80u >> (i % 8));

        std::memcpy(&payload[i / 8], &byte, 1);
    }

    session->connection->send_message(
        PeerMessageType::BITFIELD,
        std::move(payload),
        [this, session](asio::error_code ec, Peer&, bool ignored){
            if (ec) {
                request_peer_disconnect(session, ec);
                return;
            }
            if (ignored) return;
        }
    );
}

void Engine::handle_event_impl(PeerBitmaskUpdate event){
    auto session = event.session.lock();
    if (!session){
        return;
    }
    bool set_interested = false;
    std::visit([this, session, &set_interested](auto&& pieces_data){
        using T = std::decay_t<decltype(pieces_data)>;
        // complete bitmask
        if constexpr (std::is_same_v<T, std::vector<char>>){
            size_t n = 0;
            size_t pieces_count = session->pieces_bitmask.size();
            if (pieces_data.size() != (pieces_count+7) / 8) return;

            for (char c : pieces_data){
                for (size_t i = 1 << 7; i > 0; i >>= 1){
                    session->pieces_bitmask[n] = (c & i);
                    if (!_piece_manager.pieces_bitmask()[n] && (c & i)) set_interested = true;
                    n++;
                    if (n >= pieces_count) break;
                }
                if (n >= pieces_count) break;
            }
        } else if (std::is_same_v<T, size_t>) { // single piece index update
            if (pieces_data >= session->pieces_bitmask.size()) return;
            session->pieces_bitmask[pieces_data] = true;
            if (!_piece_manager.pieces_bitmask()[pieces_data]) set_interested = true;
        }
    }, event.pieces);
    if (set_interested) 
        session->connection->set_interested(
            true,
            [this, session](asio::error_code ec, Peer&, bool){
                if (ec) request_peer_disconnect(session, ec);
            }
        );
}

void Engine::handle_event_impl(Engine::PeerChangedState event){

}

void Engine::handle_event_impl(Engine::PieceBlockReceived event){
    auto& pieces_in_progress = _piece_manager.pieces_in_progress();
    if (pieces_in_progress.find(event.piece_idx) == pieces_in_progress.end()){
        return; // we are not currently downloading this piece
    }
    auto& piece = _piece_manager.update_piece(
        event.piece_idx,
        event.offset,
        {event.data.data(), event.data.size()}
    );
    if (piece.is_downloaded){
        bool succeeded = _piece_manager.verify_hash(event.piece_idx);
        if (succeeded) {
            _piece_manager.store_piece(event.piece_idx);
            std::array<char, 12> message_payload{};
            char* msg_ptr = message_payload.data();
            utils::write_net_buffer<uint32_t>(msg_ptr, event.piece_idx);
            for (auto session : _peers) {
                session->connection->send_message(
                    PeerMessageType::HAVE,
                    message_payload,
                    [this, session](asio::error_code ec, Peer&, bool){
                        if (ec) request_peer_disconnect(session, ec);
                    }
                );
            }
        }
        // in both cases try to download next piece (maybe retry):

        return;
    }
    size_t piece_length = piece.data.size();
    auto session = event.session.lock();
    if (!session || !session->is_alive.load()) return;
    if (!session->connection->am_choked()){
        auto& ranges = pieces_in_progress.at(event.piece_idx);
        if (ranges.empty()) return;

        auto last_range_it = std::prev(ranges.end());
        size_t requested_blocks_end = last_range_it->second.first;
        if (requested_blocks_end < piece_length){
            size_t next_block_length = (piece_length - requested_blocks_end) >= MAX_BLOCK_LENGTH ?
                                       MAX_BLOCK_LENGTH : (piece_length - requested_blocks_end);
            std::array<char, 12> message_payload{};
            char* msg_ptr = message_payload.data();
            utils::write_net_buffer<uint32_t>(msg_ptr, event.piece_idx);
            utils::write_net_buffer<uint32_t>(msg_ptr, requested_blocks_end);
            utils::write_net_buffer<uint32_t>(msg_ptr, next_block_length);
            session->connection->send_message(
                PeerMessageType::REQUEST,
                message_payload,
                [this, session](asio::error_code ec, Peer&, bool){
                    if (ec) request_peer_disconnect(session, ec);
                }
            );
        }
    }
}

void Engine::handle_event_impl(Engine::PieceBlockRequested event){

}

void Engine::handle_event_impl(Engine::PieceDownloadExpected event){

}

void Engine::schedule_requests(){

}

void Engine::update_choking(){

}

void Engine::update_rates(){

}

bool Engine::is_download_complete() const {

}

}
