#pragma once

#include "TraceSchema.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace goalx {

/* Indicating `count` appendix entries starting at entry `first`. */
struct EdgeRange {
    std::uint32_t count, first;
};

template <class D>
concept Decoder =
    requires(const D& d, const std::byte* rec, typename D::node_type& node, const Schema& s) {
        typename D::node_type; // What read_node fills.
        { D::create(s) } -> std::same_as<std::expected<D, std::string>>;
        { d.read_node(rec, node) } -> std::same_as<void>;
        { d.depcnt(rec) } -> std::same_as<std::uint32_t>;
        { d.done_edges(rec) } -> std::same_as<EdgeRange>;
        { d.start_edges(rec) } -> std::same_as<EdgeRange>;
        { D::name } -> std::convertible_to<const char*>; // Used for messages and benchmarks.
    };

} // namespace goalx