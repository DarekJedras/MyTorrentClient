#ifndef UTILS_HPP
#define UTILS_HPP

#include <random>
#include <bit>
#include <cstring>
#include <vector>
#include <openssl/evp.h>
#include <array>
#include <string_view>

namespace torrent::utils {

    template<typename T>
    T get_random_bytes(){
        thread_local std::mt19937_64 gen(std::random_device{}());
        std::uniform_int_distribution<T> dist;
        return dist(gen);
    }

    template <typename T>
    void write_net_buffer(char*& buf_ptr, T int_value){
        if constexpr (std::endian::native == std::endian::little){
            int_value = std::byteswap(int_value);
        }

        std::memcpy(buf_ptr, &int_value, sizeof(T));
        buf_ptr += sizeof(T);
    }

    template <typename T>
    T read_net_buffer(const char*& buf_ptr) {
        T value;
        std::memcpy(&value, buf_ptr, sizeof(T)); 
        buf_ptr += sizeof(T);

        if constexpr (std::endian::native == std::endian::little) {
            value = std::byteswap(value);
        }
        return value;
    }

    // returns true if selected bit was set
    inline bool check_bitmask(size_t idx, const std::vector<char>& bitmask){
        return bitmask.at(idx / 8) & (1 << (idx & 0x7));
    }

    // returns the previous state of changed bit (true if set)
    inline bool set_bitmask(bool value, size_t idx, std::vector<char>& bitmask){
        bool prev_state = bitmask.at(idx / 8) & (1 << (idx & 0x7));
        if (value) {
            bitmask.at(idx / 8) |= (1 << (idx & 0x7));
        } else {
            bitmask.at(idx / 8) &= ~(1 << (idx & 0x7));
        }
        return prev_state;
    }

    inline std::array<char, 20> compute_sha1(std::string_view data){
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
            EVP_MD_CTX_new(), 
            &EVP_MD_CTX_free
        );
        if (!ctx) {
            throw std::runtime_error("couldn't create EVP_MD_CTX context");
        }

        if (EVP_DigestInit_ex(ctx.get(), EVP_sha1(), nullptr) != 1) {
            throw std::runtime_error("couldn't initialize SHA-1 algorithm");
        }

        if (EVP_DigestUpdate(ctx.get(), data.data(), data.size()) != 1) {
            throw std::runtime_error("couldn't update SHA-1 digest state");
        }

        std::array<char, 20> hash;
        unsigned int hash_len = 0;

        if (EVP_DigestFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(hash.data()), &hash_len) != 1) {
            throw std::runtime_error("couldn't finalize SHA-1 digest");
        }

        if (hash_len != hash.size()){
            throw std::runtime_error("unexpected SHA-1 digest length");
        }

        return hash;
    }

} // namespace torrent::utils

#endif
