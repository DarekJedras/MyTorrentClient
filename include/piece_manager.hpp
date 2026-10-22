#ifndef PIECE_MANAGER_HPP
#define PIECE_MANAGER_HPP

#include <vector>
#include <span>
#include <cstdint>
#include <fstream>
#include <optional>
#include <filesystem>
#include "torrent_file.hpp"

namespace torrent {

class DiskStorage {
    std::fstream _temp_file;
    std::filesystem::path _temp_file_path;
public:
    explicit DiskStorage(const std::filesystem::path& path);

    void resize_file(std::uintmax_t size);

    std::vector<char> read_piece_from_disk(
        size_t piece_idx,
        const TorrentFile& torrent_info,
        std::optional<std::vector<char>> piece_storage
    );

    bool store_piece_on_disk(size_t piece_idx, const TorrentFile& torrent_info, std::span<char> piece);
};

struct PieceObject {
    std::vector<char> data;
    bool is_downloaded = false; // means whole piece is downloaded
    bool is_valid = false; // means piece is downloaded and hash is verified
    bool is_stored = false; // means piece is stored on the disk
    bool is_loaded = false; // means .data contains piece data (not necessarily complete data)
};

class PieceManager {
    TorrentFile _torrent_info;
    DiskStorage _disk_io;
    std::vector<PieceObject> _pieces_storage;
    std::vector<bool> _pieces_bitmask;

public:
    PieceManager(TorrentFile torrent, DiskStorage disk_io);

    // returns reference to stored piece object
    const PieceObject& read_piece(size_t idx) const;

    // sets given vector as data container for selected piece (resizes it if necessary)
    // and loads piece from disk if it is downloaded,
    // returns true if valid piece was loaded from the disk
    bool load_piece(size_t idx, std::optional<std::vector<char>> piece_storage);

    // sets given vector as data container for selected piece (resizes it if necessary)
    void add_piece(size_t idx, std::vector<char> piece);
    void update_piece(size_t idx, size_t offset, std::span<char> data, bool make_complete);

    // returns piece container (containing current piece data) and sets piece's data container empty
    std::vector<char> drop_piece(size_t idx);

    void store_piece(size_t idx);

    // returns true if hash verification completed succesfully
    bool verify_hash(size_t idx);

    const std::vector<bool>& pieces_bitmask() const {return _pieces_bitmask;}
};

} // namespace torrent

#endif
