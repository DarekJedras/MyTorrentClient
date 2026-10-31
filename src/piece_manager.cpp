#include "piece_manager.hpp"
#include <utility>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <optional>
#include <vector>
#include <span>
#include <iostream>
#include <iterator>
#include "utils.hpp"

namespace torrent {

namespace {

size_t calculate_piece_size(size_t idx, const TorrentFile& torrent_info){
    if (20 * idx < torrent_info.pieces().size() - 20){
        return torrent_info.piece_length();
    }
    size_t last_piece_length = torrent_info.total_file_length() % torrent_info.piece_length();
    return (last_piece_length == 0) ? torrent_info.piece_length() : last_piece_length;
}

}

PieceManager::PieceManager(TorrentFile torrent_info, DiskStorage disk_io) : 
    _torrent_info(std::move(torrent_info)), _disk_io(std::move(disk_io)),
    _pieces_storage(_torrent_info.pieces().length() / 20),
    _pieces_bitmask(_torrent_info.pieces().length() / 20)
    {
        _disk_io.resize_file(_torrent_info.total_file_length());
}

const PieceObject& PieceManager::read_piece(size_t idx) const {
    return _pieces_storage.at(idx);
}

void PieceManager::add_piece(size_t idx, std::vector<char> piece_data){
    auto& piece = _pieces_storage.at(idx);
    piece.data = std::move(piece_data);
    piece.data.resize(calculate_piece_size(idx, _torrent_info));
    piece.is_loaded = true;
}

const PieceObject& PieceManager::update_piece(
    size_t idx,
    size_t offset,
    std::span<char> data
) {
    auto& piece = _pieces_storage.at(idx);

    if (offset > piece.data.size()
        || data.size() > piece.data.size() - offset
       ){
        throw std::out_of_range("data exceeds given piece size");
    }

    auto ranges_it = _pieces_in_progress.find(idx);
    if (ranges_it == _pieces_in_progress.end()){
        return piece;
    }

    auto& piece_ranges = ranges_it->second;
    const size_t end = offset + data.size();

    auto it = piece_ranges.find(offset);
    if (it == piece_ranges.end()
        || it->second.first != end
        || it->second.second != RangeState::REQUESTED
       ){
        return piece;
    }

    // copy received data
    std::memcpy(piece.data.data() + offset, data.data(), data.size());
    // mark range as received
    it->second.second = RangeState::RECEIVED;

    // merge with previous range
    auto merged_it = it;
    if (merged_it != piece_ranges.begin()) {
        auto prev_it = std::prev(merged_it);

        if (prev_it->second.first == merged_it->first
            && prev_it->second.second == RangeState::RECEIVED
           ){
            prev_it->second.first = merged_it->second.first;
            piece_ranges.erase(merged_it);
            merged_it = prev_it;
        }
    }

    // merge with next range
    auto next_it = std::next(merged_it);
    if (next_it != piece_ranges.end()
        && merged_it->second.first == next_it->first
        && next_it->second.second == RangeState::RECEIVED
       ){
        merged_it->second.first = next_it->second.first;
        piece_ranges.erase(next_it);
    }

    piece.is_valid = false;
    _pieces_bitmask[idx] = false;
    piece.is_downloaded = false;

    // check whether the entire piece has been downloaded
    auto first = piece_ranges.begin();
    if (first != piece_ranges.end()
        && first->first == 0
        && first->second.first == piece.data.size()
        && first->second.second == RangeState::RECEIVED
        && std::next(first) == piece_ranges.end()
       ){
        piece.is_downloaded = true;
        _pieces_in_progress.erase(idx);
    }
    return piece;
}

void PieceManager::mark_block_requested(size_t idx, size_t offset, size_t length){
    PieceRanges& ranges = _pieces_in_progress[idx];
    // ranges should be checked against overlapping
    ranges[offset] = {offset + length, RangeState::REQUESTED};
}

bool PieceManager::verify_hash(size_t idx){
    auto& piece = _pieces_storage.at(idx);
    if (piece.data.size() == 0) return false;

    const char* expected_hash_ptr = _torrent_info.pieces().data() + 20*idx;
    Hash20 current_hash = utils::compute_sha1(std::string_view(piece.data.data(), piece.data.size()));
    if (std::memcmp(expected_hash_ptr, current_hash.data(), current_hash.size()) != 0){
        piece.is_valid = false;
        _pieces_bitmask[idx] = false;
        return false;
    }
    piece.is_valid = true;
    _pieces_bitmask[idx] = true; // index verification done at the beginning
    return true;
}

bool PieceManager::load_piece(size_t idx, std::optional<std::vector<char>> piece_storage){
    auto& piece = _pieces_storage.at(idx);
    if (!piece.is_stored){
        if (piece_storage.has_value()){
            piece.data = std::move(piece_storage.value());
        }
        piece.data.resize(calculate_piece_size(idx, _torrent_info));
        return false;
    }
    piece.data = _disk_io.read_piece_from_disk(idx, _torrent_info, std::move(piece_storage));
    piece.is_loaded = true;
    return piece.is_valid;
}

std::vector<char> PieceManager::drop_piece(size_t idx){
    auto& piece = _pieces_storage.at(idx);
    if (piece.is_valid && !piece.is_stored){
        bool result = _disk_io.store_piece_on_disk(idx, _torrent_info, std::span(piece.data.data(), piece.data.size()));
        piece.is_stored = result;
        if (!result) throw std::runtime_error("couldn't write piece to the disk");
    }
    piece.is_loaded = false;
    auto piece_container = std::move(piece.data);
    piece.data = std::vector<char>{};
    _pieces_in_progress.erase(idx);
    return piece_container;
}

void PieceManager::store_piece(size_t idx){
    auto& piece = _pieces_storage.at(idx);
    if (piece.is_valid && !piece.is_stored){
        bool result = _disk_io.store_piece_on_disk(idx, _torrent_info, std::span(piece.data.data(), piece.data.size()));
        piece.is_stored = result;
        if (!result) throw std::runtime_error("couldn't write piece to the disk");
    }
}

DiskStorage::DiskStorage(const std::filesystem::path& path)
    : _temp_file(path, std::ios::binary | std::ios::in | std::ios::out),
      _temp_file_path(path)
      {
    if (!std::filesystem::exists(path)) {
        std::ofstream create(path, std::ios::binary);
        if (!create)
            throw std::runtime_error("Cannot create temporary file");
    }

    _temp_file.open(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!_temp_file){
        std::cerr << _temp_file.flags();
        throw std::runtime_error("Cannot open temporary file");
    }
}

void DiskStorage::resize_file(std::uintmax_t size){
    _temp_file.flush();

    std::filesystem::resize_file(_temp_file_path, size);
}

std::vector<char> DiskStorage::read_piece_from_disk(
    size_t idx,
    const TorrentFile& torrent_info,
    std::optional<std::vector<char>> piece_storage
){
    size_t piece_size = calculate_piece_size(idx, torrent_info);
    std::vector<char> storage;
    if (piece_storage.has_value()){
        storage = std::move(piece_storage.value());
    }
    if (storage.size() != piece_size){
        storage.resize(piece_size);
    }
    _temp_file.seekg(torrent_info.piece_length() * idx);
    _temp_file.read(storage.data(), piece_size);
    if (_temp_file.gcount() != piece_size){
        throw std::runtime_error("couldn't read piece from the disk");
    }
    return storage;
}

bool DiskStorage::store_piece_on_disk(size_t piece_idx, const TorrentFile& torrent_info, std::span<char> piece){
    _temp_file.seekp(torrent_info.piece_length() * piece_idx);
    _temp_file.write(piece.data(), piece.size());

    return static_cast<bool>(_temp_file);
}

} // namespace torrent
