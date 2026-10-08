#include "peer.hpp"
#include "utils.hpp"
#include "asio.hpp"
#include <cstdint>
#include <utility>
#include <vector>
#include <array>
#include "utils.hpp"

namespace torrent {

Peer::Peer(asio::ip::tcp::socket&& socket) : _socket(std::move(socket)){
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
    });
}

void Peer::store_received_msg(asio::error_code ec, size_t){
    if (ec){
        _ec = ec;
        return;
    }
    ShortMsgBuffer& buf = std::get<ShortMsgBuffer>(_recv_queue.back());
    const char* msg_buf_ptr = buf.data();
    uint32_t msg_length = utils::read_net_buffer<uint32_t>(msg_buf_ptr);
    if (msg_length == 0){ // keep alive
        asio::async_read(
            _socket,
            asio::buffer(buf.data(), 4),
            [this](asio::error_code ec, size_t n){
                if (ec){
                    _ec = ec;
                    return;
                }
                // keep alive messages are ignored
                this->store_received_msg(ec, n);
            }
        );
    } else if (msg_length <= buf.size() - 4){ // short msg
        asio::async_read(
            _socket,
            asio::buffer(buf.data() + 4, msg_length),
            [this](asio::error_code ec, size_t n){
                if (ec){
                    _ec = ec;
                    return;
                }
                auto& next_buf = std::get<ShortMsgBuffer>(_recv_queue.emplace_back());
                asio::async_read(
                    _socket,
                    asio::buffer(next_buf.data(), 4),
                    [this](asio::error_code ec, size_t n){
                        this->store_received_msg(ec, n);
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
            [this](asio::error_code ec, size_t n){
                if (ec){
                    _ec = ec;
                    return;
                }
                auto& next_buf = std::get<ShortMsgBuffer>(_recv_queue.emplace_back());
                asio::async_read(
                    _socket,
                    asio::buffer(next_buf.data(), 4),
                    [this](asio::error_code ec, size_t n){
                        this->store_received_msg(ec, n);
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
        auto& first_buf = std::get<ShortMsgBuffer>(_recv_queue.emplace_back());
        auto& second_buf = std::get<ShortMsgBuffer>(_recv_queue.emplace_back());
        std::array<asio::mutable_buffer, 2> buffers{
            asio::buffer(first_buf),
            asio::buffer(second_buf.data(), 8)
        };
        asio::async_read(_socket, buffers, [handler, this](asio::error_code ec, size_t){
            if (ec){
                _ec = ec;
                return handler(ec, {}, {}, *this);
            }
            constexpr std::array<char, 20> protocol_start = {
                19, 'B', 'i', 't', 'T', 'o', 'r', 'r', 'e', 'n', 't',
                ' ', 'p', 'r', 'o', 't', 'o', 'c', 'o', 'l'
            };
            auto& first_buf = std::get<ShortMsgBuffer>(_recv_queue.front());
            auto& second_buf = std::get<ShortMsgBuffer>(*(++_recv_queue.begin()));
            const char* second_buf_ptr = second_buf.data();
            auto second_buf_value = utils::read_net_buffer<uint64_t>(second_buf_ptr);
            if (first_buf != protocol_start || second_buf_value != 0){
                _ec = asio::error::operation_not_supported;
                return handler(asio::error::operation_not_supported, {}, {}, *this);
            }
            std::array<asio::mutable_buffer, 2> buffers{
                asio::buffer(first_buf),
                asio::buffer(second_buf)
            };
            asio::async_read(_socket, buffers, [handler, this](asio::error_code ec, size_t){
                if (ec){
                    _ec = ec;
                }
                auto& first_buf = std::get<ShortMsgBuffer>(_recv_queue.front());
                auto& second_buf = std::get<ShortMsgBuffer>(*(++_recv_queue.begin()));
                handler(ec, first_buf, second_buf, *this);
                _recv_queue.clear();
                _recv_queue.emplace_back();
                asio::async_read(
                    _socket,
                    asio::buffer(std::get<ShortMsgBuffer>(_recv_queue.back()).data(), 4),
                    [this](asio::error_code ec, size_t n){
                        this->store_received_msg(ec, n);
                    }
                );
            });
        });
    });
}

void Peer::read_message(RecvHandler handler){
    asio::post(_socket.get_executor(), [this, handler = std::move(handler)](){
        if (_ec){
            return handler(_ec, {}, *this, false);
        }
        if (_pending_read.has_value()){
            return handler(_ec, {}, *this, true);
        } else if(_recv_queue.size() <= 1){
            _pending_read = std::move(handler);
            return;
        }
        run_read_handler(handler);
    });
}

void Peer::send_stored_msg(asio::error_code ec, size_t){
    if (ec){
        _ec = ec;
        return;
    }
    auto& [message_data, handler] = _send_queue.front();
    std::visit([&handler, this](auto& buffer){
        const char* buffer_ptr = buffer.data();
        uint32_t message_length = utils::read_net_buffer<uint32_t>(buffer_ptr);
        asio::async_write(
            _socket,
            asio::buffer(buffer.data(), message_length + 4),
            [this, &handler](asio::error_code ec, size_t n){
                if (ec){
                    _ec = ec;
                }
                handler(ec, n);
                _send_queue.pop_front();
                if (!_ec && !_send_queue.empty()){
                    send_stored_msg(ec, n);
                }
            }
        );
    }, message_data);
}

void Peer::set_interested(bool value, ErrorHandler handler){
    asio::post(_socket.get_executor(), [this, value, handler = std::move(handler)](){
        if (_ec || _am_interested == value){
            return handler(_ec, *this);
        }
        // if there are other messages in queue, it will be sent after them
        bool need_send_call = _send_queue.empty();
        auto& queue_slot = _send_queue.emplace_back();
        auto& buf = std::get<ShortMsgBuffer>(queue_slot.first);
        char* buf_ptr = buf.data();
        utils::write_net_buffer<uint32_t>(buf_ptr, 1);
        *buf_ptr = std::to_underlying(value ? PeerMessageType::INTERESTED : PeerMessageType::NOT_INTERESTED);
        queue_slot.second = [this, value, handler = std::move(handler)](asio::error_code ec, size_t n){
            if (!ec){
                _am_interested = value;
            }
            handler(ec, *this);
        };
        if (need_send_call){
            send_stored_msg(asio::error_code(), 0);
        }
    });
}

void Peer::set_choked(bool value, ErrorHandler handler){
    asio::post(_socket.get_executor(), [this, value, handler = std::move(handler)](){
        if (_ec || _is_choked == value){
            return handler(_ec, *this);
        }
        // if there are other messages in queue, it will be sent after them
        bool need_send_call = _send_queue.empty();
        auto& queue_slot = _send_queue.emplace_back();
        auto& buf = std::get<ShortMsgBuffer>(queue_slot.first);
        char* buf_ptr = buf.data();
        utils::write_net_buffer<uint32_t>(buf_ptr, 1);
        *buf_ptr = std::to_underlying(value ? PeerMessageType::CHOKE : PeerMessageType::UNCHOKE);
        queue_slot.second = [this, value, handler = std::move(handler)](asio::error_code ec, size_t n){
            if (!ec){
                _is_choked = value;
            }
            handler(ec, *this);
        };
        if (need_send_call){
            send_stored_msg(asio::error_code(), 0);
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
            handler(_ec, {}, *this, false);
            return;
        }
        PeerMessageType type = static_cast<PeerMessageType>(type_value);
        std::span<char> payload(msg_buf.data() + 5, length - 1);
        update_internal_state(type);
        handler(_ec, {payload, length, type}, *this, false);
    }, _recv_queue.front());
    _recv_queue.pop_front();
}

}
