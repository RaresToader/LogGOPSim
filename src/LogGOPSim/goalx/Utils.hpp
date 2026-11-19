#pragma once

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <print>
#include <sys/stat.h>
#include <type_traits>

namespace goalx {

inline std::size_t get_file_size(FILE* fd) {
    struct stat f_info;
    if (fstat(fileno(fd), &f_info)) {
        std::println(stderr, "Cannot read file length!");
        std::exit(EXIT_FAILURE);
    }
    return f_info.st_size;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
inline void get(const std::byte* buf, size_t& pos, T& v) {
    std::memcpy(&v, buf + pos, sizeof(T));
    pos += sizeof(T);
}

inline bool is_host_little_endian() { return std::endian::native == std::endian::little; }

} // namespace goalx