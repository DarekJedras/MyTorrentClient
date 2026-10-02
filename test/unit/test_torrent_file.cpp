#include <gtest/gtest.h>
#include <stdexcept>
#include "torrent_file.hpp"


// ============================================================================
// TorrentFile CONSTRUCTOR TESTS
// ============================================================================

TEST(TorrentFileTest, ParseSingleFileTorrentSuccessfully) {
    std::string single_file_bencode = 
        "d8:announce35:https://torrent.ubuntu.com/announce"
        "4:infod6:lengthi330056e4:name30:ubuntu-24.04-desktop-amd64.iso"
        "12:piece lengthi262144e6:pieces40:12345678901234567890"
        "09876543210987654321ee";

    torrent::TorrentFile torrent(single_file_bencode);

    EXPECT_EQ(torrent.announce(), "https://torrent.ubuntu.com/announce");
    EXPECT_FALSE(torrent.directory_name().has_value());
    EXPECT_EQ(torrent.piece_length(), 262144);
    EXPECT_EQ(torrent.pieces(), "1234567890123456789009876543210987654321");
    
    ASSERT_EQ(torrent.files_spec().size(), 1);
    EXPECT_EQ(torrent.files_spec()[0].filepath(), "ubuntu-24.04-desktop-amd64.iso");
    EXPECT_EQ(torrent.files_spec()[0].length(), 330056LL);
}

TEST(TorrentFileTest, ParseMultiFileTorrentSuccessfully) {
    std::string multi_file_bencode = 
        "d8:announce35:https://torrent.ubuntu.com/announce"
        "4:infod5:filesld6:lengthi1024e4:pathl11:release.txteed6:lengthi242880e4:pathl8:docs_pdf10:manual.pdfee"
        "e4:name12:ubuntu-files12:piece lengthi262144e6:pieces20:12345678901234567890ee";

    torrent::TorrentFile torrent(multi_file_bencode);

    EXPECT_EQ(torrent.announce(), "https://torrent.ubuntu.com/announce");
    ASSERT_TRUE(torrent.directory_name().has_value());
    EXPECT_EQ(torrent.directory_name().value(), "ubuntu-files");
    EXPECT_EQ(torrent.piece_length(), 262144);

    ASSERT_EQ(torrent.files_spec().size(), 2);

    EXPECT_EQ(torrent.files_spec()[0].filepath(), std::filesystem::path("release.txt"));
    EXPECT_EQ(torrent.files_spec()[0].length(), 1024);

    EXPECT_EQ(torrent.files_spec()[1].filepath(), std::filesystem::path("docs_pdf") / "manual.pdf");
    EXPECT_EQ(torrent.files_spec()[1].length(), 242880);
}

TEST(TorrentFileTest, ThrowsOnInvalidBencodeFormat) {
    std::string malformed_bencode = "d8:announce36:invalid_data_without_end";

    EXPECT_THROW(torrent::TorrentFile torrent(malformed_bencode), std::runtime_error);
}

TEST(TorrentFileTest, ThrowsOnMissingRequiredKeys) {
    std::string missing_pieces_bencode = 
        "d8:announce10:http://a.c4:infod6:lengthi100e4:name1:ae";

    EXPECT_THROW(torrent::TorrentFile torrent(missing_pieces_bencode), std::runtime_error);
}