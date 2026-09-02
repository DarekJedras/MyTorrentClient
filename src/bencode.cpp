#include <variant>
#include <string>
#include <vector>
#include <map>
#include <exception>
#include <iostream>
#include "bencode.hpp"


BencodeValue parse_bencode_value(const std::string& bencoded_text, size_t& begin_pos);

BencodeString parse_bencode_string(const std::string& bencoded_text, size_t& begin_pos){
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

BencodeInt parse_bencode_int(const std::string& bencoded_text, size_t& begin_pos){
    size_t end_pos = bencoded_text.find('e', begin_pos);
    if (end_pos == std::string::npos || bencoded_text[begin_pos] != 'i') throw ParsingError(begin_pos);

    BencodeInt integer_value;
    try {
        integer_value = std::stoll(bencoded_text.substr(begin_pos + 1, end_pos - begin_pos - 1));
    }
    catch (...) {
        throw ParsingError(begin_pos);
    }

    begin_pos = end_pos + 1;
    return integer_value;
}

BencodeList parse_bencode_list(const std::string& bencoded_text, size_t& begin_pos){
    if (bencoded_text[begin_pos] != 'l') throw ParsingError(begin_pos);
    size_t current_pos = begin_pos + 1;
    BencodeList list = BencodeList();
    try {
        while (bencoded_text[current_pos] != 'e'){
            BencodeValue value = parse_bencode_value(bencoded_text, current_pos);
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

BencodeDict parse_bencode_dict(const std::string& bencoded_text, size_t& begin_pos){
    if (bencoded_text[begin_pos] != 'd') throw ParsingError(begin_pos);

    size_t current_pos = begin_pos + 1;
    BencodeDict dict = BencodeDict();
    while (bencoded_text[current_pos] != 'e'){
        BencodeString key = parse_bencode_string(bencoded_text, current_pos);
        BencodeValue value = parse_bencode_value(bencoded_text, current_pos);
        dict[key] = value;
    }

    begin_pos = current_pos + 1;
    return dict;
}

BencodeValue parse_bencode_value(const std::string& bencoded_text, size_t& begin_pos){
    switch (bencoded_text[begin_pos]){
        case 'i': {
            return parse_bencode_int(bencoded_text, begin_pos);
        }
        case 'l': {
            return parse_bencode_list(bencoded_text, begin_pos);
        }
        case 'd': {
            return parse_bencode_dict(bencoded_text, begin_pos);
        }
        default: {
            return parse_bencode_string(bencoded_text, begin_pos);
        }
    }
}

BencodeValue parse_bencode(const std::string& bencoded_text){
    size_t begin_pos = 0;
    return parse_bencode_value(bencoded_text, begin_pos);
}

std::string serialize_bencode(const BencodeString& bencode_string){
    return std::to_string(bencode_string.length()) + ':' + bencode_string;
}

std::string serialize_bencode(const BencodeInt& bencode_integer){
    return 'i' + std::to_string(bencode_integer) + 'e';
}

std::string serialize_bencode(const BencodeList& bencode_list){
    std::string str("l");
    for (const BencodeValue& value : bencode_list){
        str += serialize_bencode(value);
    }
    return str + 'e';
}
// If the implementation of BencodeDict changes, keys might need to be sorted explicitly
std::string serialize_bencode(const BencodeDict& bencode_dict){
    std::string str("d");
    for (const auto& [key, value] : bencode_dict){
        str += serialize_bencode(key) + serialize_bencode(value);
    }
    return str + 'e';
}

std::string serialize_bencode(const BencodeValue& bencode_value){
    return std::visit([](const auto& arg) {
        return serialize_bencode(arg);
    }, bencode_value);
}
