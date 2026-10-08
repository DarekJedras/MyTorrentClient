#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <map>
#include "bencode.hpp"

namespace {

// ============================================================================
// PARSER TESTS
// ============================================================================

TEST(BencodeParseTest, ParsesIntegerCorrectly) {
    std::string input = "i42e";
    BencodeValue result;
    ASSERT_NO_THROW(result = bdecode(input));

    const auto* val = std::get_if<BencodeInt>(&result);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(*val, 42);
}

TEST(BencodeParseTest, ParsesNegativeInteger) {
    std::string input = "i-100e";
    BencodeValue result;
    ASSERT_NO_THROW(result = bdecode(input));

    const auto* val = std::get_if<BencodeInt>(&result);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(*val, -100);
}

TEST(BencodeParseTest, ParsesStringCorrectly) {
    std::string input = "4:wiki";
    BencodeValue result;
    ASSERT_NO_THROW(result = bdecode(input));

    const auto* val = std::get_if<BencodeString>(&result);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(*val, "wiki");
}

TEST(BencodeParseTest, ParsesListCorrectly) {
    std::string input = "l4:spami42ee";
    BencodeValue result;
    ASSERT_NO_THROW(result = bdecode(input));

    const auto* list_ptr = std::get_if<BencodeList>(&result);
    ASSERT_NE(list_ptr, nullptr);
    ASSERT_EQ(list_ptr->size(), 2);

    const auto* elem0 = std::get_if<BencodeString>(&((*list_ptr)[0]));
    ASSERT_NE(elem0, nullptr);
    EXPECT_EQ(*elem0, "spam");

    const auto* elem1 = std::get_if<BencodeInt>(&((*list_ptr)[1]));
    ASSERT_NE(elem1, nullptr);
    EXPECT_EQ(*elem1, 42);
}

TEST(BencodeParseTest, ThrowsErrorOnInvalidFormat) {
    std::string invalid_input = "i42";
    
    EXPECT_THROW(bdecode(invalid_input), std::exception);
}

TEST(BencodeParseTest, ThrowsErrorOnIntegerWithLeadingZeros) {
    std::string invalid_input = "i04212e";
    
    EXPECT_THROW(bdecode(invalid_input), std::exception);
}

TEST(BencodeParseTest, ThrowsErrorOnIntegerWithLeadingSpace) {
    std::string invalid_input = "i 4212e";
    
    EXPECT_THROW(bdecode(invalid_input), std::exception);
}

TEST(BencodeParseTest, ThrowsErrorOnInvalidZeroInteger) {
    std::string invalid_input = "i-0e";
    
    EXPECT_THROW(bdecode(invalid_input), std::exception);
}

TEST(BencodeParseTest, ThrowsErrorOnInvalidIntegerFormat) {
    std::string invalid_input = "i+2e";
    
    EXPECT_THROW(bdecode(invalid_input), std::exception);
}

TEST(BencodeParseTest, ThrowsErrorOnInvalidDict) {
    std::string invalid_input = "d4:spami42e3:cowl1:a1:bee";
    
    EXPECT_THROW(bdecode(invalid_input), std::exception);
}

// ============================================================================
// SERIALIZATION TESTS
// ============================================================================

TEST(BencodeSerializeTest, SerializesInteger) {
    BencodeValue val = BencodeInt{123};
    EXPECT_EQ(bencode(val), "i123e");
}

TEST(BencodeSerializeTest, SerializesString) {
    BencodeValue val = BencodeString{"hello"};
    EXPECT_EQ(bencode(val), "5:hello");
}

TEST(BencodeSerializeTest, SerializesDictionaryWithSortedKeys) {
    BencodeDict dict;
    dict["spam"] = BencodeString{"eggs"};
    dict["cow"] = BencodeString{"moo"};

    BencodeValue val = dict;

    std::string expected = "d3:cow3:moo4:spam4:eggse";
    EXPECT_EQ(bencode(val), expected);
}

// ============================================================================
// ADDITIONAL TESTS
// ============================================================================

TEST(BencodeRoundTripTest, ParseAndSerializeRestoresOriginalString) {
    std::string original = "d3:cowi42e4:spaml1:a1:bee";
    
    BencodeValue parsed = bdecode(original);
    std::string reserialized = bencode(parsed);

    EXPECT_EQ(reserialized, original);
}

} // namespace
