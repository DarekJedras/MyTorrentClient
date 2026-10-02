#include "udp_tracker.hpp"
#include "utils.hpp"
#include <bit>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <iostream> // tests only

namespace torrent::udp {

constexpr size_t UDP_BUFFER_SIZE = 1024;

std::array<char, 16> ConnectRequest::serialize() const {
    std::array<char, 16> buffer;
    char* buf_ptr = buffer.data();

    utils::write_net_buffer(buf_ptr, protocol_id);
    utils::write_net_buffer(buf_ptr, action);
    utils::write_net_buffer(buf_ptr, transaction_id);
    return buffer;
}

bool ConnectResponse::deserialize(const char* data, size_t bytes_length){
    if (bytes_length < 16){
        return false;
    }
    action = utils::read_net_buffer<uint32_t>(data);
    transaction_id = utils::read_net_buffer<uint32_t>(data);
    if (action == 0){
        connection_id = utils::read_net_buffer<uint64_t>(data);
    } else if (action == 3){
        error_msg = std::string(data, bytes_length - 8);
    }
    else {
        return false;
    }
    return true;
}

std::optional<ConnectResponse> get_connect_response(asio_udp::socket& socket){
    asio::error_code ec;
    socket.remote_endpoint(ec);
    if (!socket.is_open() || ec){
        throw std::logic_error("socket must be opened in valid protocol and connected to endpoint");
    }
    ConnectRequest request = {
        .transaction_id = utils::get_random_bytes<uint32_t>()
    };
    socket.send(asio::buffer(request.serialize()));

    asio_udp::endpoint sender_endpoint; // to be deleted
    std::array<char, UDP_BUFFER_SIZE> recv_buffer;
    ConnectResponse response;
    size_t recv_bytes = socket.receive_from(asio::buffer(recv_buffer), sender_endpoint);
    if (response.deserialize(recv_buffer.data(), recv_bytes)){
        std::cout << "Received datagram from " << sender_endpoint << " of size " << recv_bytes << std::endl;
        if (response.transaction_id == request.transaction_id){
            return response;
        }
    }
    return std::optional<ConnectResponse>();
}

std::array<char, 98> AnnounceRequest::serialize() const {
    std::array<char, 98> buffer;
    char* buf_ptr = buffer.data();

    utils::write_net_buffer(buf_ptr, connection_id);
    utils::write_net_buffer(buf_ptr, action);
    utils::write_net_buffer(buf_ptr, transaction_id);
    std::memcpy(buf_ptr, info_hash.data(), 20);
    buf_ptr += 20;
    std::memcpy(buf_ptr, peer_id.data(), 20);
    buf_ptr += 20;
    utils::write_net_buffer(buf_ptr, downloaded);
    utils::write_net_buffer(buf_ptr, left);
    utils::write_net_buffer(buf_ptr, uploaded);
    utils::write_net_buffer(buf_ptr, event);
    utils::write_net_buffer(buf_ptr, ip_addr);
    utils::write_net_buffer(buf_ptr, key);
    utils::write_net_buffer(buf_ptr, num_want);
    utils::write_net_buffer(buf_ptr, port);
    return buffer;
}

bool AnnounceResponse::deserialize(const char* data, size_t bytes_length, asio_udp proto){
    if (
        bytes_length < 20 ||
        (proto == asio_udp::v4() && (bytes_length - 20) % 6 != 0) ||
        (proto == asio_udp::v6() && (bytes_length - 20) % 18 != 0)
        ){
        return false;
    }

    const char* data_end = data + bytes_length;
    action = utils::read_net_buffer<uint32_t>(data);
    transaction_id = utils::read_net_buffer<uint32_t>(data);
    if (action == 3){
        error_msg = std::string(reinterpret_cast<const char*>(data), bytes_length - 8);
        return true;
    } else if (action != 1){
        return false;
    }
    interval = utils::read_net_buffer<uint32_t>(data);
    leechers = utils::read_net_buffer<uint32_t>(data);
    seeders = utils::read_net_buffer<uint32_t>(data);
    if (proto == asio_udp::v4()){
        PeersListv4 peers;
        while (data < data_end){
            uint32_t ip_addr = utils::read_net_buffer<uint32_t>(data);
            uint16_t port = utils::read_net_buffer<uint16_t>(data);
            peers.emplace_back(ip_addr, port);
        }
        peers_list = peers;
    }
    else {
        PeersListv6 peers;
        while (data < data_end){
            std::array<uint8_t, 16> ip_addr;
            std::memcpy(&ip_addr, data, 16);
            data += 16;
            uint16_t port = utils::read_net_buffer<uint16_t>(data);
            peers.emplace_back(ip_addr, port);
        }
        peers_list = peers;
    }
    return true;
}

std::optional<AnnounceResponse> get_announce_response(const AnnounceRequest& request, asio_udp::socket& socket){
    asio::error_code ec;
    asio_udp proto = socket.remote_endpoint(ec).protocol();
    if (!socket.is_open() || ec){
        throw std::logic_error("socket must be opened in valid protocol and connected to endpoint");
    }

    std::array<char, 98> request_data = request.serialize();
    size_t bytes_send = socket.send(asio::buffer(request_data));
    std::cout << "sent " << bytes_send << " bytes of announce request" << std::endl;

    asio_udp::endpoint sender_endpoint; // to be deleted
    std::array<char, UDP_BUFFER_SIZE> recv_buffer;
    AnnounceResponse response;
    size_t recv_bytes = socket.receive_from(asio::buffer(recv_buffer), sender_endpoint);
    if (response.deserialize(recv_buffer.data(), recv_bytes, proto)){
        std::cout << "Received datagram from " << sender_endpoint << " of size " << recv_bytes << std::endl;
        if (response.transaction_id == request.transaction_id){
            return response;
        }
    }
    return std::optional<AnnounceResponse>();
}

AnnounceResponse get_tracker_response(const ParsedURL& parsed_url, const TrackerRequest& request){
    AnnounceRequest request_data = {
        .info_hash = request.info_hash,
        .peer_id = request.peer_id,
        .downloaded = request.downloaded,
        .left = request.left,
        .uploaded = request.uploaded,
        .event = std::to_underlying(request.event),
        .key = request.key,
        .port = request.port
    };

    asio::io_context io;
    auto endpoints = asio_udp::resolver(io).resolve(parsed_url.host, std::to_string(parsed_url.port));

    asio_udp::socket socket(io);
    for (asio_udp::endpoint tracker_endpoint : endpoints){
        try {
            socket.open(tracker_endpoint.protocol());
            socket.connect(tracker_endpoint);
            return get_connect_response(socket)
                .and_then([&socket, &request_data](ConnectResponse conn_response){
                    if (conn_response.action == 3){
                        return std::optional<AnnounceResponse>({
                            .error_msg = conn_response.error_msg.value()
                        });
                    }
                    request_data.connection_id = conn_response.connection_id;
                    request_data.transaction_id = utils::get_random_bytes<uint32_t>();
                    return get_announce_response(request_data, socket);
                }).value();
        } catch (...){
            socket.close();
        }
    }
    throw std::runtime_error("Couldn't reach tracker");
}

TrackerResponse parse_tracker_response(const AnnounceResponse& data){
    TrackerResponse response;
    if (data.error_msg.has_value()){
            response.failure_reason = data.error_msg;
            return response;
    }
    response.interval = static_cast<long long>(data.interval);
    std::visit(
        [&response](auto&& peers_list) {
            response.peers.reserve(peers_list.size());

            using ListType = std::decay_t<decltype(peers_list)>;
            using AddrType = std::conditional_t<
                std::is_same_v<ListType, PeersListv4>,
                asio::ip::address_v4,
                asio::ip::address_v6
            >;

            for (const auto& [ip_num, port] : peers_list) {
                AddrType addr(ip_num);

                response.peers.push_back(PeerInfo{
                    .ip = addr.to_string(),
                    .port = port
                });
            }
        },
        data.peers_list
    );

    return response;
}

} // namespace torrent::udp
