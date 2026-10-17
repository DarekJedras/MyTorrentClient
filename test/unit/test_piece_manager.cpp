#include <gtest/gtest.h>
#include "piece_manager.hpp"
#include "utils.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <initializer_list>

namespace torrent {
namespace {

std::string bencode_string(std::string_view value){
    return std::to_string(value.size()) + ":" + std::string(value);
}

std::string make_file_entry(
    size_t length,
    std::initializer_list<std::string_view> path)
    {
    std::string result = "d";
    result += bencode_string("length");
    result += "i" + std::to_string(length) + "e";
    result += bencode_string("path");
    result += "l";

    for (auto component : path)
        result += bencode_string(component);

    result += "ee"; // koniec listy path i słownika pliku
    return result;
}

std::string make_multifile_torrent(){
    constexpr std::string_view data = "abcdefghij";
    constexpr size_t piece_length = 4;

    std::string pieces;

    for (size_t offset = 0; offset < data.size(); offset += piece_length) {
        auto chunk = data.substr(
            offset,
            std::min(piece_length, data.size() - offset));

        auto hash = utils::compute_sha1(chunk);
        pieces.append(hash.data(), hash.size());
    }

    std::string info = "d";

    info += bencode_string("files");
    info += "l";
    info += make_file_entry(2, {"a.bin"});
    info += make_file_entry(3, {"dir", "b.bin"});
    info += make_file_entry(5, {"c.bin"});
    info += "e"; // koniec listy files

    info += bencode_string("name") + bencode_string("root");
    info += bencode_string("piece length") + "i4e";
    info += bencode_string("pieces") + bencode_string(pieces);
    info += "e"; // koniec info

    std::string torrent = "d";
    torrent += bencode_string("announce");
    torrent += bencode_string("http://tracker.invalid/announce");
    torrent += bencode_string("info");
    torrent += info;
    torrent += "e"; // koniec torrenta

    return torrent;
}

std::filesystem::path make_temp_path(){
    static std::atomic_uint64_t counter{0};

    auto id = std::chrono::steady_clock::now()
                  .time_since_epoch()
                  .count();

    return std::filesystem::temp_directory_path() /
        ("piece_manager_test_" + std::to_string(id) + "_" +
         std::to_string(counter.fetch_add(1)) + ".tmp");
}

class DiskStorageTest : public ::testing::Test {
protected:
    std::string torrent_data{make_multifile_torrent()};
    TorrentFile torrent_info{torrent_data};
    std::filesystem::path temp_path{make_temp_path()};
    std::unique_ptr<DiskStorage> disk_io;

    void SetUp() override {
        disk_io = std::make_unique<DiskStorage>(temp_path);
    }

    void TearDown() override {
        disk_io.reset();

        std::error_code ec;
        std::filesystem::remove(temp_path, ec);
    }
};

TEST_F(DiskStorageTest, RoundTripsAllPiecesIncludingLastPartialPiece){
    std::array<char, 4> first{'a', 'b', 'c', 'd'};
    std::array<char, 4> second{'e', 'f', 'g', 'h'};
    std::array<char, 2> last{'i', 'j'};

    ASSERT_TRUE(disk_io->store_piece_on_disk(
        0, torrent_info, std::span<char>{first}));

    ASSERT_TRUE(disk_io->store_piece_on_disk(
        1, torrent_info, std::span<char>{second}));

    ASSERT_TRUE(disk_io->store_piece_on_disk(
        2, torrent_info, std::span<char>{last}));

    EXPECT_EQ(
        disk_io->read_piece_from_disk(0, torrent_info, std::nullopt),
        (std::vector<char>{'a', 'b', 'c', 'd'})
    );

    EXPECT_EQ(
        disk_io->read_piece_from_disk(1, torrent_info, std::nullopt),
        (std::vector<char>{'e', 'f', 'g', 'h'})
    );

    EXPECT_EQ(
        disk_io->read_piece_from_disk(2, torrent_info, std::nullopt),
        (std::vector<char>{'i', 'j'})
    );
}

class PieceManagerTest : public ::testing::Test {
protected:
    std::string torrent_data{make_multifile_torrent()};
    std::filesystem::path temp_path{make_temp_path()};
    std::unique_ptr<PieceManager> manager;

    void SetUp() override {
        manager = std::make_unique<PieceManager>(
            TorrentFile{torrent_data},
            DiskStorage{temp_path});
    }

    void TearDown() override {
        manager.reset();

        std::error_code ec;
        std::filesystem::remove(temp_path, ec);
    }
};

TEST_F(PieceManagerTest, CalculatesLastPieceSizeCorrectly){
    manager->add_piece(2, std::vector<char>{'i', 'j'});

    const auto& piece = manager->read_piece(2);

    EXPECT_EQ(piece.data.size(), 2U);
    EXPECT_TRUE(piece.is_loaded);
}

TEST_F(PieceManagerTest, VerifiesCorrectPieceHash){
    manager->add_piece(0, std::vector<char>{'a', 'b', 'c', 'd'});

    EXPECT_TRUE(manager->verify_hash(0));
    EXPECT_TRUE(manager->read_piece(0).is_valid);
}

TEST_F(PieceManagerTest, VerifyHashRejectsCorruptedPiece){
    manager->add_piece(0, std::vector<char>{'a', 'b', 'c', 'd'});
    ASSERT_TRUE(manager->verify_hash(0));

    std::array<char, 1> corrupted{'X'};
    manager->update_piece(0, 0, std::span<char>{corrupted}, false);

    EXPECT_FALSE(manager->verify_hash(0));
    EXPECT_FALSE(manager->read_piece(0).is_valid);
}

TEST_F(PieceManagerTest, UpdatePieceRejectsOutOfBoundsWrite){
    manager->add_piece(0, std::vector<char>{'a', 'b', 'c', 'd'});

    std::array<char, 2> patch{'X', 'Y'};

    EXPECT_THROW(
        manager->update_piece(0, 3, std::span<char>{patch}, false),
        std::out_of_range
    );
}

TEST_F(PieceManagerTest, StoresDropsAndReloadsPiece){
    const std::vector<char> expected{'a', 'b', 'c', 'd'};

    manager->add_piece(0, expected);
    ASSERT_TRUE(manager->verify_hash(0));

    manager->store_piece(0);
    ASSERT_TRUE(manager->read_piece(0).is_stored);

    EXPECT_EQ(manager->drop_piece(0), expected);
    EXPECT_FALSE(manager->read_piece(0).is_loaded);

    EXPECT_TRUE(manager->load_piece(0, std::nullopt));

    const auto& piece = manager->read_piece(0);
    EXPECT_TRUE(piece.is_loaded);
    EXPECT_EQ(piece.data, expected);
}

} // namespace
} // namespace torrent
