#ifndef TORRENT_FILE_HPP
#define TORRENT_FILE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

struct FileSpec {
    std::filesystem::path filepath;
    int64_t length;

    explicit FileSpec(std::filesystem::path filepath, int64_t length)
        : filepath(std::move(filepath)), length(length){}
};

struct TorrentFile {
    std::string announce;
    std::optional<std::filesystem::path> directory_name;
    std::string pieces;
    std::vector<FileSpec> files_spec;
    int64_t piece_length = 0;

    explicit TorrentFile(const std::string& bencoded_data);
};

TorrentFile read_torrent_file(const std::filesystem::path& file_name);
std::string get_file_description(const TorrentFile& file, bool print_pieces = false);

#endif
