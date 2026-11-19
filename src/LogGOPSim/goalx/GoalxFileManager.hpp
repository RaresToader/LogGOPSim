#pragma once

#include "TraceSchema.hpp"
#include "goalx/GoalxRequirements.hpp"
#include "goalx/GoalxUtils.hpp"

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <print>
#include <sys/mman.h>
#include <type_traits>

namespace goalx {

class GoalxFileManager {
  public:
    GoalxFileManager(const std::filesystem::path& file);

    ~GoalxFileManager();

    const Schema& schema() const { return schema_; }
    const std::byte* mapping_start() const { return mapping_start_; }
    std::uint32_t max_cpu() const { return max_cpu_; }
    std::uint32_t max_nic() const { return max_nic_; }
    std::uint32_t num_ranks() const { return num_ranks_; }
    const std::byte* jump_table_start() const { return jump_table_start_; }

  private:
    const std::byte* mapping_start_;
    const std::byte* jump_table_start_;
    std::size_t mapping_length_;
    Schema schema_;
    std::uint32_t num_ranks_;
    std::uint32_t max_cpu_;
    std::uint32_t max_nic_;
};

GoalxFileManager::GoalxFileManager(const std::filesystem::path& file) {
    using clock = std::chrono::steady_clock;
    clock::time_point start_parse_header = clock::now();

    std::FILE* fd = std::fopen(file.c_str(), "rb");
    if (fd == NULL) {
        std::println(stderr, "Couldn't open input file {}!", file.c_str());
        std::exit(EXIT_FAILURE);
    }
    mapping_length_ = get_file_size(fd);
    if (mapping_length_ < GOALX_OFF_FIELDS) {
        std::println(stderr, "File is too short for a GOALX header!");
        std::exit(EXIT_FAILURE);
    }

    mapping_start_ =
        static_cast<std::byte*>(mmap(NULL, mapping_length_, PROT_READ, MAP_PRIVATE, fileno(fd), 0));
    if (mapping_start_ == MAP_FAILED) {
        std::println(stderr, "Couldn't mmap the trace!");
        std::exit(EXIT_FAILURE);
    }
    std::fclose(fd);

    std::size_t pos = 0;
    std::uint64_t magic_cookie, hash;
    std::uint32_t header_len;
    std::uint16_t version, num_fields, num_meta;
    std::uint8_t endianness;

    get(mapping_start_, pos, magic_cookie);
    if (magic_cookie != GOALX_MAGIC) {
        std::println(stderr, "Expected GOALX magic cookie {} corresponding, but got {}!",
                     GOALX_MAGIC, magic_cookie);
        std::exit(EXIT_FAILURE);
    }

    get(mapping_start_, pos, version);
    if (version != GOALX_VERSION) {
        std::println(stderr, "Expected GOALX version {}, but got {}!", GOALX_VERSION, version);
        std::exit(EXIT_FAILURE);
    }

    get(mapping_start_, pos, endianness);
    if (endianness != GOALX_ENDIAN_LITTLE || !is_host_little_endian()) {
        std::println(stderr, "Expected little endianness!");
        std::exit(EXIT_FAILURE);
    }

    get(mapping_start_, pos, schema_.flags);

    get(mapping_start_, pos, header_len);
    if (header_len < GOALX_OFF_FIELDS || header_len > mapping_length_) {
        std::println(stderr, "Header length field out of bounds!");
        std::exit(EXIT_FAILURE);
    }

    get(mapping_start_, pos, hash);
    get(mapping_start_, pos, num_ranks_);
    get(mapping_start_, pos, max_cpu_);
    get(mapping_start_, pos, max_nic_);
    get(mapping_start_, pos, num_fields);
    get(mapping_start_, pos, schema_.record_bits);
    get(mapping_start_, pos, schema_.record_bytes);
    get(mapping_start_, pos, num_meta);

    get(mapping_start_, pos, schema_.edge_bytes);
    if (!(schema_.edge_bytes >= 1 && schema_.edge_bytes <= 4)) {
        std::println(stderr, "The edge bytes must be between 1 and 4!");
        std::exit(EXIT_FAILURE);
    }
    pos += 7;

    if (pos != GOALX_OFF_FIELDS) {
        std::println(stderr, "Mismatch between file manager and header size!");
        std::exit(EXIT_FAILURE);
    }

    for (std::uint16_t i = 0; i < num_fields; i++) {
        FieldDesc f;
        std::uint8_t name_len, reserved;
        if (pos + GOALX_RECORD_LEN > header_len) {
            std::println(stderr, "Truncated record!");
            std::exit(EXIT_FAILURE);
        }
        get(mapping_start_, pos, f.kind);
        get(mapping_start_, pos, f.bit_offset);
        get(mapping_start_, pos, f.width);
        get(mapping_start_, pos, f.flags);
        get(mapping_start_, pos, f.op_mask);
        get(mapping_start_, pos, reserved);
        get(mapping_start_, pos, f.dflt);
        get(mapping_start_, pos, f.max_value);
        get(mapping_start_, pos, name_len);
        if (pos + name_len > header_len) {
            std::println(stderr, "Truncated field name!");
            std::exit(EXIT_FAILURE);
        }
        f.name = std::string(reinterpret_cast<const char*>(mapping_start_ + pos), name_len);
        pos += name_len;
        if (schema_.find(f.name) != nullptr) {
            std::println(stderr, "Field name {} declared twice!", f.name);
            std::exit(EXIT_FAILURE);
        }
        schema_.fields.push_back(f);
    }

    for (std::uint16_t i = 0; i < num_meta; i++) {
        std::uint16_t key_len, value_len;
        if (pos + sizeof(key_len) > header_len) {
            std::println(stderr, "Truncated metadata!");
            std::exit(EXIT_FAILURE);
        }
        get(mapping_start_, pos, key_len);
        if (pos + key_len + sizeof(value_len) > header_len) {
            std::println(stderr, "Truncated metadata!");
            std::exit(EXIT_FAILURE);
        }
        std::string key(reinterpret_cast<const char*>(mapping_start_ + pos), key_len);
        pos += key_len;
        get(mapping_start_, pos, value_len);
        if (pos + value_len > header_len) {
            std::println(stderr, "Truncated metadata!");
            std::exit(EXIT_FAILURE);
        }
        schema_.meta.push_back(std::make_pair(
            key, std::string(reinterpret_cast<const char*>(mapping_start_ + pos), value_len)));
        pos += value_len;
    }

    if (schema_.record_bytes != (schema_.record_bits + 7) / 8) {
        std::println(stderr, "Record size does not match record bits!");
        std::exit(EXIT_FAILURE);
    }
    if (schema_.hash() != hash) {
        std::println(stderr, "Schema hash mismatch, the header is corrupt!");
        std::exit(EXIT_FAILURE);
    }
    if (header_len + sizeof(uint64_t) * 2 * num_ranks_ > mapping_length_) {
        std::println(stderr, "Jump table extends past the end of the file!");
        std::exit(EXIT_FAILURE);
    }
    jump_table_start_ = mapping_start_ + header_len;

    clock::time_point end_parse_header = clock::now();
    auto parse_time_sec =
        std::chrono::duration<double>(end_parse_header - start_parse_header).count();
    std::println("Header parse time {:.5f} seconds", parse_time_sec);
}

GoalxFileManager::~GoalxFileManager() {
    if (munmap(const_cast<std::byte*>(mapping_start_), mapping_length_)) {
        std::println("Cannot unmmap the file!");
        std::exit(EXIT_FAILURE);
    }
}

} // namespace goalx