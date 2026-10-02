#include "tracker.hpp"
#include "httplib.h"
#include "asio.hpp"
#include "udp_tracker.hpp"
#include "utils.hpp"
#include "peer.hpp"
#include <optional>

namespace torrent {

std::string_view to_string(TrackerEvent e) noexcept {
    switch (e) {
        case TrackerEvent::Completed:   return "completed";
        case TrackerEvent::Started:     return "started";
        case TrackerEvent::Stopped:     return "stopped";
        case TrackerEvent::None:        return "empty";
    }
    return "";
}

Tracker::Tracker(const std::string& announce_url){
    size_t proto_end_pos = announce_url.find(':');
    if (proto_end_pos == std::string::npos){
        throw std::runtime_error("protocol not specified");
    }
    if (announce_url[proto_end_pos + 1] != '/' || announce_url[proto_end_pos + 2] != '/'){
        throw std::runtime_error("invalid syntax: host delimiter should be '://'");
    }
    url_.protocol = announce_url.substr(0, proto_end_pos);

    size_t host_begin_pos = proto_end_pos + 3;
    size_t host_end_pos = announce_url.find(':', host_begin_pos);
    size_t port_end_pos;
    if (host_end_pos != std::string::npos){
        port_end_pos = announce_url.find('/', host_end_pos + 1);
        std::string port_str = announce_url.substr(
            host_end_pos + 1,
            port_end_pos == std::string::npos ? std::string::npos : port_end_pos - (host_end_pos + 1)
        );
        try {
            size_t converted_chars;
            int port = std::stoi(port_str, &converted_chars);
            if (port < 0 || port > UINT16_MAX || converted_chars != port_str.length()){
                throw std::invalid_argument("");
            }
            url_.port = port;
        }
        catch (...){
            throw std::runtime_error("invalid port number");
        }
    } else {
        host_end_pos = announce_url.find('/', host_begin_pos);
        port_end_pos = host_end_pos;
        if (host_end_pos == std::string::npos){
            throw std::runtime_error("host not specified");
        }
        // choose default port number
        if (url_.protocol == "https"){
            url_.port = 443;
        } else if (url_.protocol == "http"){
            url_.port = 80;
        } else {
            throw std::runtime_error("cannot deduce port number");
        }
    }
    url_.host = announce_url.substr(host_begin_pos, host_end_pos - host_begin_pos);
    if (port_end_pos == std::string::npos){
        url_.path = "/";
        return;
    }
    size_t path_end_pos = announce_url.find('?', port_end_pos);
    url_.path = announce_url.substr(
        port_end_pos,
        path_end_pos == std::string::npos ? std::string::npos : path_end_pos - port_end_pos
    );
}

std::string Tracker::url() const {
    return url_.protocol + "://" + url_.host + ':' + std::to_string(url_.port) + url_.path;
}

std::vector<PeerInfo> Tracker::parse_peers_list(const BencodeValue& peers_bencoded, bool is_peers6){
    return std::visit([is_peers6](auto&& peers_list){
        using BencodeType = std::decay_t<decltype(peers_list)>;
    
        std::vector<PeerInfo> peers;
        if constexpr(std::is_same_v<BencodeType, BencodeString>){
            if ((!is_peers6 && peers_list.size() % 6 != 0) ||
                (is_peers6 && peers_list.size() % 18 != 0)){
                throw std::runtime_error("invalid peers list format");
            }

            const uint8_t* data = reinterpret_cast<const uint8_t*>(peers_list.data());
            const uint8_t* end = data + peers_list.size();
            while (data < end){
                std::string ip_str;
                if (is_peers6){
                    std::array<uint8_t, 16> ip_bytes;
                    std::memcpy(&ip_bytes, data, 16);
                    data += 16;
                    ip_str = asio::ip::address_v6(ip_bytes).to_string();
                } else {
                    uint32_t ip_bytes = utils::read_net_buffer<uint32_t>(data);
                    ip_str = asio::ip::address_v4(ip_bytes).to_string();
                }

                uint16_t port = utils::read_net_buffer<uint16_t>(data);
                peers.emplace_back(std::move(ip_str), std::optional<PeerId>(), port);
            }
        } else if constexpr(std::is_same_v<BencodeType, BencodeList>){
            try{
                for (const auto& value : peers_list){
                    const BencodeDict& peer_dict = std::get<BencodeDict>(value);
                    const BencodeString& ip = extract_bencode_value<BencodeString>("ip", peer_dict);
                    const BencodeString& peer_id = extract_bencode_value<BencodeString>("peer id", peer_dict);
                    if (peer_id.size() != 20){
                        throw std::runtime_error("invalid peers list format");
                    }
                    PeerId peer_id_arr{};
                    std::memcpy(&peer_id_arr, peer_id.data(), 20);

                    BencodeInt port = extract_bencode_value<BencodeInt>("port", peer_dict);
                    peers.emplace_back(ip, peer_id_arr, port);
                }
            }
            catch (...){
                throw std::runtime_error("invalid peers list format");
            }
        } else {
            throw std::runtime_error("invalid peers list format");
        }
        return peers;
    }, peers_bencoded);
}

inline std::string get_tracker_response(const ParsedURL& url, const TrackerRequest& request){
    httplib::Params params{
        {"info_hash", std::string(request.info_hash.data(), 20)},
        {"peer_id", std::string(request.peer_id.data(), 20)},
        {"port", std::to_string(request.port)},
        {"uploaded", std::to_string(request.uploaded)},
        {"downloaded", std::to_string(request.downloaded)},
        {"left", std::to_string(request.left)},
        {"key", std::to_string(request.key)}
    };
    if (request.event != TrackerEvent::None){
        params.emplace("event", to_string(request.event));
    }

    httplib::Client cli(url.protocol + "://" + url.host + ':' + std::to_string(url.port));
    httplib::Result result = cli.Get(std::string(url.path), params);

    if (!result) {
        throw std::runtime_error("couldn't reach tracker");
    }
    if (result.value().status >= 400) {
        throw std::runtime_error("couldn't reach tracker: code " + std::to_string(result.value().status));
    }
    return result.value().body;
}

TrackerResponse parse_tracker_response(const BencodeDict& response){
    if (response.contains("failure reason")){
        const BencodeString& message = extract_bencode_value<BencodeString>("failure reason", response);
        return {
            .peers = std::vector<PeerInfo>(),
            .failure_reason = message
        };
    }
    const BencodeInt& interval = extract_bencode_value<BencodeInt>("interval", response);

    std::vector<PeerInfo> peers;
    if (response.contains("peers6"))
        peers = Tracker::parse_peers_list(response.at("peers6"), true);
    else 
        peers = Tracker::parse_peers_list(response.at("peers"), false);

    return {
        .interval = interval,
        .peers = std::move(peers)
    };
}

TrackerResponse Tracker::announce(const TrackerRequest& request) const {
    if (url_.protocol == "http" || url_.protocol == "https"){
        std::string bencoded_data = get_tracker_response(url_, request);
        BencodeValue data = bdecode(bencoded_data);
        BencodeDict* data_ptr = std::get_if<BencodeDict>(&data);
        if (!data_ptr){
            throw std::runtime_error("invalid tracker response");
        }
        return parse_tracker_response(*data_ptr);
    } else if (url_.protocol == "udp"){
        udp::AnnounceResponse data = udp::get_tracker_response(url_, request);
        return udp::parse_tracker_response(data);
    } else {
        throw std::runtime_error("unknown communication protocol");
    }
}

} // namespace torrent
