#include "peer.hpp"
#include "utils.hpp"
#include "asio.hpp"
#include <cstdint>
#include <utility>
#include <vector>
#include <array>
#include <chrono>
#include "utils.hpp"
#include <iostream>

using namespace std::chrono_literals;

namespace torrent {

namespace {
uint32_t deduce_msg_length(PeerMessageType type){
    using torrent::PeerMessageType;
    switch (type) {
        case PeerMessageType::CHOKE:
            return 1;
        case PeerMessageType::UNCHOKE:
            return 1;
        case PeerMessageType::INTERESTED:
            return 1;
        case PeerMessageType::NOT_INTERESTED:
            return 1;
        case PeerMessageType::HAVE:
            return 5;
        case PeerMessageType::REQUEST:
            return 13;
        case PeerMessageType::CANCEL:
            return 13;
        default:
            return 0;
    }
}
}

constexpr std::array<char, 20> PROTOCOL_HEADER = {
    19, 'B', 'i', 't', 'T', 'o', 'r', 'r', 'e', 'n', 't',
    ' ', 'p', 'r', 'o', 't', 'o', 'c', 'o', 'l'
};

Peer::Peer(asio::ip::tcp::socket&& socket) :
        _socket(std::move(socket)), 
        _sender_timer(_socket.get_executor()),
        _receiver_timer(_socket.get_executor())
    {
    asio::error_code ec;
    _socket.remote_endpoint(ec);
    if (ec){
        _ec = asio::error::not_connected;
    }
}

void Peer::disconnect(){
    asio::post(_socket.get_executor(), [this]{
        _socket.shutdown(asio::socket_base::shutdown_both, _ec);
        _socket.close(_ec);
        _ec = asio::error::not_connected;

        _sender_timer.cancel();
        _receiver_timer.cancel();
    });
}

void Peer::store_received_msg(){
    if (_ec){
        return;
    }
    set_keep_alive_check();
    ShortMsgBuffer& buf = std::get<ShortMsgBuffer>(_recv_queue.back());
    const char* msg_buf_ptr = buf.data();
    uint32_t msg_length = utils::read_net_buffer<uint32_t>(msg_buf_ptr);
    if (msg_length == 0){ // keep alive
        asio::async_read(
            _socket,
            asio::buffer(buf.data(), 4),
            [this](asio::error_code ec, size_t){
                if (ec){
                    _ec = ec;
                    return;
                }
                // keep alive messages are ignored
                this->store_received_msg();
            }
        );
    } else if (msg_length <= buf.size() - 4){ // short msg
        asio::async_read(
            _socket,
            asio::buffer(buf.data() + 4, msg_length),
            [this](asio::error_code ec, size_t){
                if (ec){
                    _ec = ec;
                    return;
                }
                auto& next_buf = std::get<ShortMsgBuffer>(_recv_queue.emplace_back());
                asio::async_read(
                    _socket,
                    asio::buffer(next_buf.data(), 4),
                    [this](asio::error_code ec, size_t){
                        if (ec){
                            _ec = ec;
                            return;
                        }
                        this->store_received_msg();
                    }
                );
                if (_pending_read.has_value()){
                    run_read_handler(_pending_read.value());
                    _pending_read.reset();
                }
            }
        );
    } else { // long msg (vector buffered)
        auto& long_buffer = _recv_queue.back().emplace<std::vector<char>>(msg_length + 4);
        char* long_buffer_ptr = long_buffer.data();
        utils::write_net_buffer<uint32_t>(long_buffer_ptr, msg_length);
        asio::async_read(
            _socket,
            asio::buffer(long_buffer_ptr, msg_length),
            [this](asio::error_code ec, size_t){
                if (ec){
                    _ec = ec;
                    return;
                }
                auto& next_buf = std::get<ShortMsgBuffer>(_recv_queue.emplace_back());
                asio::async_read(
                    _socket,
                    asio::buffer(next_buf.data(), 4),
                    [this](asio::error_code ec, size_t){
                        if (ec){
                            _ec = ec;
                            return;
                        }
                        this->store_received_msg();
                    }
                );
                if (_pending_read.has_value()){
                    run_read_handler(_pending_read.value());
                    _pending_read.reset();
                }
            }
        );
    }
}

void Peer::read_handshake(HandshakeHandler handler){
    asio::post(_socket.get_executor(), [this, handler = std::move(handler)](){
        if (_read_handshake_started){
            if (handler) handler(_ec, {}, {}, *this, true);
            return;
        }
        _read_handshake_started = true;
        auto& first_buf = std::get<ShortMsgBuffer>(_recv_queue.emplace_back());
        auto& second_buf = std::get<ShortMsgBuffer>(_recv_queue.emplace_back());
        std::array<asio::mutable_buffer, 2> buffers{
            asio::buffer(first_buf),
            asio::buffer(second_buf.data(), 8)
        };
        asio::async_read(_socket, buffers, [handler, this](asio::error_code ec, size_t){
            if (ec){
                _ec = ec;
                if (handler) handler(ec, {}, {}, *this, false);
                return;
            }
            auto& first_buf = std::get<ShortMsgBuffer>(_recv_queue[0]);
            auto& second_buf = std::get<ShortMsgBuffer>(_recv_queue[1]);
            const char* second_buf_ptr = second_buf.data();
            auto second_buf_value = utils::read_net_buffer<uint64_t>(second_buf_ptr);
            if (first_buf != PROTOCOL_HEADER || second_buf_value != 0){
                _ec = asio::error::operation_not_supported;
                if (handler) handler(asio::error::operation_not_supported, {}, {}, *this, false);
                return;
            }
            std::array<asio::mutable_buffer, 2> buffers{
                asio::buffer(first_buf),
                asio::buffer(second_buf)
            };
            asio::async_read(_socket, buffers, [handler, this](asio::error_code ec, size_t){
                if (ec){
                    _ec = ec;
                }
                auto& first_buf = std::get<ShortMsgBuffer>(_recv_queue[0]);
                auto& second_buf = std::get<ShortMsgBuffer>(_recv_queue[1]);
                if (handler) handler(ec, first_buf, second_buf, *this, false);
                _recv_queue.clear();
                _recv_queue.emplace_back();
                _recv_queue_locked = false;
                asio::async_read(
                    _socket,
                    asio::buffer(std::get<ShortMsgBuffer>(_recv_queue.back()).data(), 4),
                    [this](asio::error_code ec, size_t){
                        if (ec){
                            _ec = ec;
                            return;
                        }
                        this->store_received_msg();
                    }
                );
                set_keep_alive_check();
            });
        });
    });
}

void Peer::read_message(RecvHandler handler){
    asio::post(_socket.get_executor(), [this, handler = std::move(handler)](){
        if (_ec){
            if (handler) handler(_ec, {}, *this, false);
            return;
        }
        if (_pending_read.has_value() || _recv_queue_locked){
            if (handler) handler(_ec, {}, *this, true);
            return;
        } else if(_recv_queue.size() <= 1){
            _pending_read = std::move(handler);
            return;
        }
        run_read_handler(handler);
    });
}

void Peer::send_stored_msg(){
    if (_ec){
        return;
    }
    auto& [message_data, callback] = _send_queue.front();
    std::visit([&callback, this](auto& buffer){
        using BufferType = std::decay_t<decltype(buffer)>;
        std::array<asio::const_buffer, 2> msg_buf;
        if constexpr (std::is_same_v<BufferType, ShortMsgBuffer>){
            const char* buffer_ptr = buffer.data();
            uint32_t message_length = utils::read_net_buffer<uint32_t>(buffer_ptr);
            msg_buf = { asio::buffer(buffer.data(), message_length + 4) };
        } else { // std::is_same_v<BufferType, LongSendBuffer>
            auto& [payload, metadata] = buffer;
            msg_buf = { asio::buffer(metadata), asio::buffer(payload) };
        }
        asio::async_write(
            _socket,
            msg_buf,
            [this, &callback](asio::error_code ec, size_t n){
                if (ec){
                    _ec = ec;
                }
                set_keep_alive_send(); // updates countdown to 2 min since now
                if (callback) callback(ec, n);
                _send_queue.pop_front();
                if (!_ec && !_send_queue.empty()){
                    this->send_stored_msg();
                }
            }
        );
    }, message_data);
}

void Peer::send_handshake(const Hash20& info_hash, const Hash20& own_id, ErrorHandler handler){
    asio::post(_socket.get_executor(), [=, this, handler = std::move(handler)]{
        if (_send_handshake_started){
            if (handler) handler(_ec, *this, true);
            return;
        }
        _send_handshake_started = true;
        _send_queue.emplace_back();
        _send_queue.emplace_back();
        auto& first_buf = std::get<ShortMsgBuffer>(_send_queue[0].first);
        auto& second_buf = std::get<ShortMsgBuffer>(_send_queue[1].first);
        first_buf = PROTOCOL_HEADER;
        second_buf = {};
        std::array<asio::const_buffer, 2> buffers{
            asio::buffer(first_buf),
            asio::buffer(second_buf.data(), 8)
        };
        asio::async_write(
            _socket,
            buffers,
            [handler = std::move(handler), this, info_hash, own_id](asio::error_code ec, size_t){
                if (ec){
                    _ec = ec;
                    if (handler) handler(ec, *this, false);
                    return;
                }
                auto& first_buf = std::get<ShortMsgBuffer>(_send_queue[0].first);
                auto& second_buf = std::get<ShortMsgBuffer>(_send_queue[1].first);
                first_buf = info_hash;
                second_buf = own_id;
                std::array<asio::const_buffer, 2> buffers{
                    asio::buffer(first_buf),
                    asio::buffer(second_buf)
                };
                asio::async_write(_socket, buffers, [this, handler = std::move(handler)](asio::error_code ec, size_t){
                    if (ec){
                        _ec = ec;
                    }
                    if (handler) handler(ec, *this, false);
                    _send_queue.clear();
                    _send_queue_locked = false;
                    set_keep_alive_send();
                });
            }
        );
    });
}

void Peer::send_message(PeerMessageType type, std::vector<char> payload, ErrorHandler handler){
    asio::post(
        _socket.get_executor(),
        [=, this, payload = std::move(payload), handler = std::move(handler)](){
            if (_ec){
                if (handler) handler(_ec, *this, false);
                return;
            } else if (_send_queue_locked){
                if (handler) handler(_ec, *this, true);
                return;
            }
            // if there are other messages in queue, it will be sent after them
            bool need_send_call = _send_queue.empty();
            auto& queue_slot = _send_queue.emplace_back();
            queue_slot.first.emplace<LongSendBuffer>(std::move(payload), std::array<char, 5>{});
            auto& [data, metadata] = std::get<LongSendBuffer>(queue_slot.first);
            char* buf_ptr = metadata.data();
            uint32_t message_length = data.size() + 1;
            utils::write_net_buffer<uint32_t>(buf_ptr, message_length);
            utils::write_net_buffer<char>(buf_ptr, std::to_underlying(type));
            queue_slot.second = [this, handler=std::move(handler)](asio::error_code ec, size_t){
                if (handler) handler(ec, *this, false);
            };
            if (need_send_call){
                send_stored_msg();
            }
        }
    );
}

void Peer::send_message(PeerMessageType type, std::array<char, 12> payload, ErrorHandler handler){
    asio::post(_socket.get_executor(), [=, this, handler = std::move(handler)](){
        if (_ec){
            if (handler) handler(_ec, *this, false);
            return;
        } else if (_send_queue_locked){
            if (handler) handler(_ec, *this, true);
            return;
        }
        // if there are other messages in queue, it will be sent after them
        bool need_send_call = _send_queue.empty();
        auto& queue_slot = _send_queue.emplace_back();
        auto& buf = std::get<ShortMsgBuffer>(queue_slot.first);
        char* buf_ptr = buf.data();
        uint32_t message_length = deduce_msg_length(type);
        utils::write_net_buffer<uint32_t>(buf_ptr, message_length);
        utils::write_net_buffer<char>(buf_ptr, std::to_underlying(type));
        std::memcpy(buf_ptr, payload.data(), message_length-1);
        queue_slot.second = [this, handler=std::move(handler)](asio::error_code ec, size_t){
            if (handler) handler(ec, *this, false);
        };
        if (need_send_call){
            send_stored_msg();
        }
    });
}

void Peer::set_interested(bool value, ErrorHandler handler){
    asio::post(_socket.get_executor(), [this, value, handler = std::move(handler)](){
        if (_ec){
            if (handler) handler(_ec, *this, false);
            return;
        } else if (_send_queue_locked){
            if (handler) handler(_ec, *this, true);
            return;
        }
        // if there are other messages in queue, it will be sent after them
        bool need_send_call = _send_queue.empty();
        auto& queue_slot = _send_queue.emplace_back();
        auto& buf = std::get<ShortMsgBuffer>(queue_slot.first);
        char* buf_ptr = buf.data();
        utils::write_net_buffer<uint32_t>(buf_ptr, 1);
        *buf_ptr = std::to_underlying(value ? PeerMessageType::INTERESTED : PeerMessageType::NOT_INTERESTED);
        queue_slot.second = [this, value, handler = std::move(handler)](asio::error_code ec, size_t){
            if (!ec){
                _am_interested = value;
            }
            if (handler) handler(ec, *this, false);
            return;
        };
        if (need_send_call){
            send_stored_msg();
        }
    });
}

void Peer::set_choked(bool value, ErrorHandler handler){
    asio::post(_socket.get_executor(), [this, value, handler = std::move(handler)](){
        if (_ec){
            if (handler) handler(_ec, *this, false);
            return;
        } else if (_send_queue_locked){
            if (handler) handler(_ec, *this, true);
            return;
        }
        // if there are other messages in queue, it will be sent after them
        bool need_send_call = _send_queue.empty();
        auto& queue_slot = _send_queue.emplace_back();
        auto& buf = std::get<ShortMsgBuffer>(queue_slot.first);
        char* buf_ptr = buf.data();
        utils::write_net_buffer<uint32_t>(buf_ptr, 1);
        *buf_ptr = std::to_underlying(value ? PeerMessageType::CHOKE : PeerMessageType::UNCHOKE);
        queue_slot.second = [this, value, handler = std::move(handler)](asio::error_code ec, size_t){
            if (!ec){
                _is_choked = value;
            }
            if (handler) handler(ec, *this, false);
        };
        if (need_send_call){
            send_stored_msg();
        }
    });
}

void Peer::update_internal_state(PeerMessageType type){
    switch (type){
        case PeerMessageType::CHOKE:
            this->_am_choked = true;
            break;
        case PeerMessageType::UNCHOKE:
            this->_am_choked = false;
            break;
        case PeerMessageType::INTERESTED:
            this->_is_interested = true;
            break;
        case PeerMessageType::NOT_INTERESTED:
            this->_is_interested = false;
            break;
        default:
            break;
    }
}

void Peer::run_read_handler(const RecvHandler& handler){
    std::visit([&handler, this](auto& msg_buf){
        const char* buf_ptr = msg_buf.data();
        uint32_t length = utils::read_net_buffer<uint32_t>(buf_ptr);
        char type_value = utils::read_net_buffer<char>(buf_ptr);
        if (type_value < 0 || type_value > 8){
            _ec = asio::error::operation_not_supported;
            if (handler) handler(_ec, {}, *this, false);
            return;
        }
        PeerMessageType type = static_cast<PeerMessageType>(type_value);
        std::span<char> payload(msg_buf.data() + 5, length - 1);
        update_internal_state(type);
        if (handler) handler(_ec, {payload, length, type}, *this, false);
    }, _recv_queue.front());
    _recv_queue.pop_front();
}

void Peer::set_keep_alive_send(){
    _sender_timer.expires_after(2min);
    _sender_timer.async_wait([this](asio::error_code ec){
        if (ec == asio::error::operation_aborted){
            return;
        } else if (ec){
            if (!_ec){
                _ec = ec;
            }
            return;
        }
        if (_send_queue.empty()){
            auto& buf = std::get<ShortMsgBuffer>(_send_queue.emplace_back().first);
            char* buf_ptr = buf.data();
            utils::write_net_buffer<uint32_t>(buf_ptr, 0);
            send_stored_msg();
        }
    });
}

void Peer::set_keep_alive_check(){
    _receiver_timer.expires_after(4min);
    _receiver_timer.async_wait([this](asio::error_code ec){
        if (ec == asio::error::operation_aborted){
            return;
        } else if (ec){
            if (!_ec){
                _ec = ec;
            }
            return;
        }
        _ec = asio::error::timed_out;
    });
}

}
