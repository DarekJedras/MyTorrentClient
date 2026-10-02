#ifndef TORRENT_FILE_HPP
#define TORRENT_FILE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace torrent {

using Hash20 = std::array<char, 20>;

class FileSpec {
    std::filesystem::path _filepath;
    int64_t _length;
public:
    const std::filesystem::path& filepath() const {return _filepath;}
    int64_t length() const {return _length;}
    explicit FileSpec(std::filesystem::path filepath, int64_t length)
        : _filepath(std::move(filepath)), _length(length){}
};

class TorrentFile {
    std::optional<std::filesystem::path> _directory_name;
    std::string _announce;
    std::string _pieces;
    std::string _info_hash;
    std::vector<FileSpec> _files_spec;
    int64_t _piece_length = 0;
public:
    const std::optional<std::filesystem::path>& directory_name() const {return _directory_name;}
    const std::string& announce() const {return _announce;}
    const std::string& pieces() const {return _pieces;}
    const std::string& info_hash() const {return _info_hash;}
    const std::vector<FileSpec>& files_spec() const {return _files_spec;}
    int64_t piece_length() const {return _piece_length;}

    explicit TorrentFile(const std::string& bencoded_data);
};

TorrentFile read_torrent_file(const std::filesystem::path& file_name);
std::string get_file_description(const TorrentFile& file, bool print_pieces = false);

} // namespace torrent

#endif
