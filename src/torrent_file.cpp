#include <fstream>
#include <format>
#include <stdexcept>
#include <openssl/evp.h>
#include<iostream>
#include "torrent_file.hpp"
#include "bencode.hpp"


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


std::filesystem::path extract_bencode_path(const BencodeList& file_path){
    std::filesystem::path path;
    for (const BencodeValue& segment : file_path){
        const BencodeString* str = std::get_if<BencodeString>(&segment);
        if (!str){
            throw std::runtime_error("path elements must be bencode strings");
        }
        path.append(*str);
    }
    return path;
}


std::string compute_sha1(std::string_view data){
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
        EVP_MD_CTX_new(), 
        &EVP_MD_CTX_free
    );
    if (!ctx) {
        throw std::runtime_error("Couldn't create EVP_MD_CTX context");
    }

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha1(), nullptr) != 1) {
        throw std::runtime_error("Couldn't initialize SHA-1 algorithm");
    }

    if (EVP_DigestUpdate(ctx.get(), data.data(), data.size()) != 1) {
        throw std::runtime_error("Couldn't update SHA-1 digest state");
    }

    std::string hash(EVP_MAX_MD_SIZE, '\0');
    unsigned int hash_len = 0;

    if (EVP_DigestFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(hash.data()), &hash_len) != 1) {
        throw std::runtime_error("Couldn't finalize SHA-1 digest");
    }

    hash.resize(hash_len);
    return hash;
}


TorrentFile::TorrentFile(const std::string& bencoded_data){
    BencodeDict torrent_data;
    try {
        BencodeValue data = bdecode(bencoded_data);
        torrent_data = std::get<BencodeDict>(data);
    } catch (const ParsingError& e){
        throw std::runtime_error(std::string("Invalid file contents: ") + e.what());
    } catch (const std::bad_variant_access&){
        throw std::runtime_error("Invalid file contents: expected bencode dict");
    }

    try {
        _announce = extract_bencode_value<BencodeString>("announce", torrent_data);
        const BencodeDict& info = extract_bencode_value<BencodeDict>("info", torrent_data);
        _info_hash = compute_sha1(bencode(info));

        _piece_length = extract_bencode_value<BencodeInt>("piece length", info);
        _pieces = extract_bencode_value<BencodeString>("pieces", info);
        if (info.contains("length") && info.contains("files")){
            throw std::runtime_error("bencode dictionary cannot contain both \"length\" and \"files\" keys");
        }
        std::filesystem::path name(extract_bencode_value<BencodeString>("name", info));
        if (info.contains("length")){
            BencodeInt length = extract_bencode_value<BencodeInt>("length", info);
            _files_spec.emplace_back(std::move(name), length);
            return;
        }
        if (info.contains("files")){
            _directory_name = std::move(name);
            const BencodeList& files = extract_bencode_value<BencodeList>("files", info);
            _files_spec.reserve(files.size());
            for (const BencodeValue& file : files){
                const BencodeDict* file_ptr = std::get_if<BencodeDict>(&file);
                if (!file_ptr){
                    throw std::runtime_error("\"files\" elements must be bencode dictionaries");
                }
                BencodeInt length = extract_bencode_value<BencodeInt>("length", *file_ptr);
                const BencodeList& path = extract_bencode_value<BencodeList>("path", *file_ptr);
                _files_spec.emplace_back(extract_bencode_path(path), length);
            }
            return;
        }
        throw std::runtime_error("missing key in bencode dictionary: \"length\" or \"files\"");
    }
    catch (const std::runtime_error& e){
        throw std::runtime_error(std::string("Ivalid file contents: ") + e.what());
    }
}


std::string read_file(const std::filesystem::path& file_name){
    std::ifstream file(file_name, std::ios::binary | std::ios::ate);
    if (!file.good()){
        throw std::runtime_error("Couldn't open file: " + file_name.string());
    }
    std::string buffer;
    const auto file_size = file.tellg();
    buffer.resize(file_size);
    file.seekg(0, std::ios::beg);
    file.read(buffer.data(), file_size);
    if (file.gcount() < file_size){
        throw std::runtime_error("Couldn't read file: " + file_name.string());
    }
    return buffer;
}


TorrentFile read_torrent_file(const std::filesystem::path& file_name){
    std::string bencoded_data = read_file(file_name);
    return TorrentFile(bencoded_data);
}


std::string bytes_to_hex(std::string_view bytes) {
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (unsigned char b : bytes){
        hex += std::format("{:02x}", b);
    }
    return hex;
}


std::string get_file_description(const TorrentFile& file, bool print_pieces){
    std::string desc = std::format(
        "URl: {}\n"
        "Info Hash: {}\n"
        "Pieces Length: {}\n",
        file.announce(), bytes_to_hex(file.info_hash()), file.piece_length()
    );

    if (!file.directory_name().has_value()){
        const auto& filespec = file.files_spec().front();
        desc += std::format("File: {} of size: {}\n", filespec.filepath().string(), filespec.length());
    } else {
        desc += std::format("Directory: {}\n\nContents:\n", file.directory_name().value().string());
        for (const auto& filespec : file.files_spec()){
            desc += std::format("File: {} of size: {}\n", filespec.filepath().string(), filespec.length());
        }
    }

    if (print_pieces){
        desc += "\nPieces: \n";
        std::string_view pieces = file.pieces();
        size_t segment_idx = 0;
        for (size_t offset = 0; offset < file.pieces().length(); offset += 20){
            desc += std::format("{:6>}: {}\n", segment_idx++, bytes_to_hex(pieces.substr(offset, 20)));
        }
    }
    return desc;
}
