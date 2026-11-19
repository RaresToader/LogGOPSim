#pragma once

#include "TraceSchema.hpp"
#include "goalx/GoalxDecoder.hpp"
#include "goalx/GoalxRequirements.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <type_traits>

namespace goalx {

/*
 * The generic decoder: Everything is decided at run time. Creating it matches
 * NodeSpec<Node> against the schema once (by field name) and keeps, for every
 * node property, the schema field's accessor and the member's offset and size.
 * read_node() then loops over those. The graph structure (depcnt, the two
 * dependents lists) is read by field kind.
 */
template <typename NodeProperties>
class GenericDecoder {
    static_assert(std::is_standard_layout_v<NodeProperties> &&
                      std::is_trivially_copyable_v<NodeProperties>,
                  "The decoder writes node properties by offset");

    static constexpr auto& field_specs = NodeSpec<NodeProperties>::fields;

  public:
    using node_type = NodeProperties;
    static constexpr const char* name = "GenericDecoder";

    /* The decoder or the error when the schema doesn't provide what NodeProperties + graph need. */
    static std::expected<GenericDecoder, std::string> create(const Schema& schema);

    /* Writes every property of NodeProperties: an absent optional field writes its default (0). */
    void read_node(const std::byte* rec, NodeProperties& node) const;

    /* The node's initial number of dependencies. */
    std::uint32_t depcnt(const std::byte* rec) const;

    /* The nodes waiting for this one to finish. */
    EdgeRange done_edges(const std::byte* rec) const;

    /* The nodes waiting for this one to start. */
    EdgeRange start_edges(const std::byte* rec) const;

  private:
    struct Slot {
        Accessor field;       // Where the value is in the record (absent: present = false).
        std::uint16_t offset; // Where it goes in the node.
        std::uint8_t size;
    };

    GenericDecoder() = default;

    static const char* chars(const std::byte* rec) { return reinterpret_cast<const char*>(rec); }

    /* The low `size` bytes of v, written without assuming the member's alignment. */
    static void store(std::byte* dst, std::uint8_t size, std::uint64_t v);

    std::array<Slot, field_specs.size()> slots_;
    Accessor depcnt_, ndep_, depix_, nsdep_, sdepix_;
};

template <class NodeProperties>
std::expected<GenericDecoder<NodeProperties>, std::string>
GenericDecoder<NodeProperties>::create(const Schema& schema) {
    GenericDecoder d;
    std::string err;
    auto fail = [&](const std::string& msg) { err += (err.empty() ? "" : "; ") + msg; };

    for (std::size_t i = 0; i < field_specs.size(); i++) {
        const FieldSpec& spec = field_specs[i];
        const FieldDesc* f = schema.find(spec.name);
        d.slots_[i] =
            Slot{f ? Accessor(f, schema.record_bytes) : Accessor(), spec.offset, spec.size};
        const std::string name = std::string("'") + spec.name + std::string("'");
        if (f == nullptr) {
            if (!(spec.flags & optional)) fail("Schema has no " + name + " field");
        } else if (f->width > 8 * spec.size) {
            fail(name + " is " + std::to_string(f->width) + " bits wide, its member holds " +
                 std::to_string(8 * spec.size));
        } else if ((f->flags & FF_ANY_SENTINEL) && spec.size < 4) {
            fail(name + " can hold ANY (0xFFFFFFFF), its member holds " +
                 std::to_string(8 * spec.size) + " bits");
        }
    }

    auto structural = [&](FieldKind kind, Accessor& out, bool needed) {
        const FieldDesc* f = schema.builtin(kind);
        const std::string name = std::string("'") + builtin_field_names[kind] + "'";
        if (f == nullptr) {
            if (needed) fail("schema has no " + name + " field");
            return;
        }
        if (f->width > 32) fail(name + " is at most 32 are supported");
        out = Accessor(f, schema.record_bytes);
    };
    structural(FK_DEPCNT, d.depcnt_, true);
    structural(FK_NDEP, d.ndep_, true);
    structural(FK_DEPIX, d.depix_, true);
    structural(FK_NSDEP, d.nsdep_, true);
    structural(FK_SDEPIX, d.sdepix_, false);

    if (!err.empty()) return std::unexpected(err);
    return d;
}

template <typename NodeProperties>
void GenericDecoder<NodeProperties>::read_node(const std::byte* rec, NodeProperties& node) const {
    auto* out = reinterpret_cast<std::byte*>(&node);
    for (const Slot& s : slots_) store(out + s.offset, s.size, s.field.get(chars(rec)));
}

template <typename NodeProperties>
std::uint32_t GenericDecoder<NodeProperties>::depcnt(const std::byte* rec) const {
    return static_cast<std::uint32_t>(depcnt_.get_simple<false>(chars(rec)));
}

template <typename NodeProperties>
EdgeRange GenericDecoder<NodeProperties>::done_edges(const std::byte* rec) const {
    return {static_cast<std::uint32_t>(ndep_.get_simple<false>(chars(rec))),
            static_cast<std::uint32_t>(depix_.get_simple<false>(chars(rec)))};
}

template <typename NodeProperties>
EdgeRange GenericDecoder<NodeProperties>::start_edges(const std::byte* rec) const {
    const char* r = chars(rec);
    std::uint32_t first =
        sdepix_.present
            ? static_cast<std::uint32_t>(sdepix_.get_simple<false>(r))
            : static_cast<std::uint32_t>(depix_.get_simple<false>(r) + ndep_.get_simple<false>(r));
    return {static_cast<std::uint32_t>(nsdep_.get_simple<false>(r)), first};
}

template <typename NodeProperties>
void GenericDecoder<NodeProperties>::store(std::byte* dst, std::uint8_t size, std::uint64_t v) {
    switch (size) {
    case 1: {
        auto x = static_cast<std::uint8_t>(v);
        std::memcpy(dst, &x, 1);
        break;
    }
    case 2: {
        auto x = static_cast<std::uint16_t>(v);
        std::memcpy(dst, &x, 2);
        break;
    }
    case 4: {
        auto x = static_cast<std::uint32_t>(v);
        std::memcpy(dst, &x, 4);
        break;
    }
    default:
        std::memcpy(dst, &v, 8);
    }
}

} // namespace goalx