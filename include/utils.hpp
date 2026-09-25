#ifndef UTILS_HPP
#define UTILS_HPP

#include <random>
#include <bit>

namespace torrent::utils {

    template<typename T>
    T get_random_bytes(){
        thread_local std::mt19937_64 gen(std::random_device{}());
        std::uniform_int_distribution<T> dist;
        return dist(gen);
    }

    template <typename T>
    void write_net_buffer(uint8_t*& buf_ptr, T int_value){
        if constexpr (std::endian::native == std::endian::little){
            int_value = std::byteswap(int_value);
        }

        std::memcpy(buf_ptr, &int_value, sizeof(T));
        buf_ptr += sizeof(T);
    }

    template <typename T>
    T read_net_buffer(const uint8_t*& buf_ptr) {
        T value;
        std::memcpy(&value, buf_ptr, sizeof(T)); 
        buf_ptr += sizeof(T);

        if constexpr (std::endian::native == std::endian::little) {
            value = std::byteswap(value);
        }
        return value;
    }
} // namespace torrent::utils

#endif
