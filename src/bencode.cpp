#include <variant>
#include <string>
#include <vector>
#include <map>
#include <exception>
#include "bencode.hpp"


BencodeValue bdecode_value(const std::string& bencoded_text, size_t& begin_pos);

BencodeString bdecode_string(const std::string& bencoded_text, size_t& begin_pos){
    size_t collon_pos = bencoded_text.find(':', begin_pos);
    if (collon_pos == std::string::npos) throw ParsingError(begin_pos);

    size_t string_value_length;
    BencodeString string_value;
    try {
        string_value_length = std::stoi(bencoded_text.substr(begin_pos, collon_pos - begin_pos));
        string_value = bencoded_text.substr(collon_pos + 1, string_value_length);
    }
    catch (const std::exception&) {
        throw ParsingError(begin_pos);
    }
    begin_pos = collon_pos + 1 + string_value_length;
    return string_value;
}

BencodeInt bdecode_int(const std::string& bencoded_text, size_t& begin_pos){
    size_t end_pos = bencoded_text.find('e', begin_pos);
    if (end_pos == std::string::npos || bencoded_text[begin_pos] != 'i') throw ParsingError(begin_pos);
    // check for leading whitespaces
    if (std::isspace(static_cast<unsigned char>(bencoded_text[begin_pos+1]))) throw ParsingError(begin_pos);
    // check for leading zeros
    if (bencoded_text[begin_pos+1] == '0' && bencoded_text[begin_pos+1] != 'e') throw ParsingError(begin_pos);
    // check for invalid combinations
    if (bencoded_text[begin_pos+1] == '+' || bencoded_text.substr(begin_pos+1, 2) == "-0") throw ParsingError(begin_pos);

    BencodeInt integer_value;
    try {
        size_t parsed_to_idx;
        integer_value = std::stoll(bencoded_text.substr(begin_pos + 1, end_pos - begin_pos - 1), &parsed_to_idx, 10);
        if (parsed_to_idx != end_pos - begin_pos - 1) throw nullptr;
    }
    catch (...) {
        throw ParsingError(begin_pos);
    }

    begin_pos = end_pos + 1;
    return integer_value;
}

BencodeList bdecode_list(const std::string& bencoded_text, size_t& begin_pos){
    if (bencoded_text[begin_pos] != 'l') throw ParsingError(begin_pos);
    size_t current_pos = begin_pos + 1;
    BencodeList list = BencodeList();
    try {
        while (bencoded_text[current_pos] != 'e'){
            BencodeValue value = bdecode_value(bencoded_text, current_pos);
            list.push_back(value);
        }
    }
    catch (const ParsingError& e) {
        throw;
    }
    catch (const std::exception& e) {
        throw ParsingError(current_pos);
    }
    begin_pos = current_pos + 1;
    return list;
}

BencodeDict bdecode_dict(const std::string& bencoded_text, size_t& begin_pos){
    if (bencoded_text[begin_pos] != 'd') throw ParsingError(begin_pos);

    size_t current_pos = begin_pos + 1;
    BencodeDict dict;
    BencodeString prev_key;
    while (bencoded_text[current_pos] != 'e'){
        size_t key_begin_pos = current_pos;
        BencodeString key = bdecode_string(bencoded_text, current_pos);
        if (key < prev_key) {
            throw ParsingError(key_begin_pos);
        } else {
            prev_key = key;
        }
        BencodeValue value = bdecode_value(bencoded_text, current_pos);
        dict[key] = value;
    }

    begin_pos = current_pos + 1;
    return dict;
}

BencodeValue bdecode_value(const std::string& bencoded_text, size_t& begin_pos){
    switch (bencoded_text[begin_pos]){
        case 'i': {
            return bdecode_int(bencoded_text, begin_pos);
        }
        case 'l': {
            return bdecode_list(bencoded_text, begin_pos);
        }
        case 'd': {
            return bdecode_dict(bencoded_text, begin_pos);
        }
        default: {
            return bdecode_string(bencoded_text, begin_pos);
        }
    }
}

BencodeValue bdecode(const std::string& bencoded_text){
    size_t begin_pos = 0;
    return bdecode_value(bencoded_text, begin_pos);
}

std::string bencode(const BencodeString& bencode_string){
    return std::to_string(bencode_string.length()) + ':' + bencode_string;
}

std::string bencode(const BencodeInt& bencode_integer){
    return 'i' + std::to_string(bencode_integer) + 'e';
}

std::string bencode(const BencodeList& bencode_list){
    std::string str("l");
    for (const BencodeValue& value : bencode_list){
        str += bencode(value);
    }
    return str + 'e';
}
// If the implementation of BencodeDict changes, keys might need to be sorted explicitly
std::string bencode(const BencodeDict& bencode_dict){
    std::string str("d");
    for (const auto& [key, value] : bencode_dict){
        str += bencode(key) + bencode(value);
    }
    return str + 'e';
}

std::string bencode(const BencodeValue& bencode_value){
    return std::visit([](const auto& arg) {
        return bencode(arg);
    }, bencode_value);
}
