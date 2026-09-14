#ifndef BENCODE_HPP
#define BENCODE_HPP

#include <variant>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <exception>
#include <stdexcept>

struct BencodeValue;

using BencodeString = std::string;
using BencodeInt = int64_t;
using BencodeList = std::vector<BencodeValue>;
using BencodeDict = std::map<std::string, BencodeValue>;

struct BencodeValue : std::variant<BencodeString, BencodeInt, BencodeList, BencodeDict> {
    using variant::variant;
};

class ParsingError : public std::exception {
    std::string message;
public:
    explicit ParsingError(int pos) {
        message = "bencode parsing error at pos: " + std::to_string(pos);
    }

    const char* what() const noexcept override {
        return message.c_str();
    }
};

BencodeValue bdecode(const std::string& bencoded_text);
std::string bencode(const BencodeValue& bencode_value);

template<typename T>
const T& extract_bencode_value(const BencodeString& key, const BencodeDict& dict){
    auto it = dict.find(key);
    if (it == dict.end()) {
        throw std::runtime_error("missing key in bencode dictionary: " + key);
    }

    if (const T* ptr = std::get_if<T>(&it->second)) {
        return *ptr;
    }

    throw std::runtime_error("invalid data type for key: " + key);
}

#endif
