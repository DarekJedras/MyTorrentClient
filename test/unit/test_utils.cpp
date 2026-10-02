#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <numeric>
#include <set>
#include "utils.hpp"

namespace torrent::utils {
namespace {

// ============================================================================
// write_net_buffer
// ============================================================================

TEST(UtilsTest, WriteNetBufferUint32) {
    std::vector<char> buffer(8, 0x00);
    char* ptr = buffer.data();

    uint32_t value = 0x12345678;
    write_net_buffer(ptr, value);

    EXPECT_EQ(buffer[0], '\x12');
    EXPECT_EQ(buffer[1], '\x34');
    EXPECT_EQ(buffer[2], '\x56');
    EXPECT_EQ(buffer[3], '\x78');

    EXPECT_EQ(ptr, buffer.data() + sizeof(uint32_t));
}

TEST(UtilsTest, WriteNetBufferSequential) {
    std::vector<char> buffer(6, 0x00);
    char* ptr = buffer.data();

    uint16_t val1 = 0xABCD;
    uint32_t val2 = 0x11223344;

    write_net_buffer(ptr, val1);
    write_net_buffer(ptr, val2);

    std::vector<char> expected = {'\xAB', '\xCD', '\x11', '\x22', '\x33', '\x44'};
    EXPECT_EQ(buffer, expected);
    EXPECT_EQ(ptr, buffer.data() + buffer.size());
}

// ============================================================================
// read_net_buffer
// ============================================================================

TEST(UtilsTest, ReadNetBufferUint32) {
    std::vector<char> buffer = {'\x12', '\x34', '\x56', '\x78', '\xFF'};
    const char* ptr = buffer.data();

    uint32_t value = read_net_buffer<uint32_t>(ptr);

    EXPECT_EQ(value, 0x12345678);
    EXPECT_EQ(ptr, buffer.data() + sizeof(uint32_t));
}

TEST(UtilsTest, ReadNetBufferSequential) {
    std::vector<char> buffer = {'\xAB', '\xCD', '\x11', '\x22', '\x33', '\x44'};
    const char* ptr = buffer.data();

    auto val1 = read_net_buffer<uint16_t>(ptr);
    auto val2 = read_net_buffer<uint32_t>(ptr);

    EXPECT_EQ(val1, 0xABCD);
    EXPECT_EQ(val2, 0x11223344);
    EXPECT_EQ(ptr, buffer.data() + buffer.size());
}

// ============================================================================
// Round-trip Test (write -> read)
// ============================================================================

template <typename T>
class NetBufferTypedTest : public ::testing::Test {};

using IntegerTypes = ::testing::Types<uint8_t, uint16_t, uint32_t, uint64_t, int16_t, int32_t, int64_t>;
TYPED_TEST_SUITE(NetBufferTypedTest, IntegerTypes);

TYPED_TEST(NetBufferTypedTest, WriteAndReadMatch) {
    TypeParam original_value = static_cast<TypeParam>(0x1F2E3D4C5B6A7988ULL);

    std::vector<char> buffer(sizeof(TypeParam));

    char* write_ptr = buffer.data();
    write_net_buffer(write_ptr, original_value);

    const char* read_ptr = buffer.data();
    TypeParam read_value = read_net_buffer<TypeParam>(read_ptr);

    EXPECT_EQ(original_value, read_value);
}

// ============================================================================
// get_random_bytes
// ============================================================================

TEST(UtilsTest, GetRandomBytesGeneratesValues) {
    constexpr int kSamples = 100;
    std::set<uint64_t> unique_values;

    for (int i = 0; i < kSamples; ++i) {
        unique_values.insert(get_random_bytes<uint64_t>());
    }

    EXPECT_GT(unique_values.size(), kSamples * 0.95);
}

TEST(UtilsTest, GetRandomBytesUint8Bounds) {
    for (int i = 0; i < 50; ++i) {
        uint8_t byte = get_random_bytes<uint8_t>();
        EXPECT_GE(byte, 0);
        EXPECT_LE(byte, 255);
    }
}

} // namespace
} // namespace torrent::utils
