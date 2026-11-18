#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace goalx {

enum : std::uint8_t { required = 0, optional = 1 };

struct FieldSpec {
    std::uint16_t offset; // of the member in the node
    std::uint8_t size;    // of the member: 1, 2, 4 or 8
    const char* name;     // of the schema field
    std::uint8_t flags;
};

/* Checks the member's type once, then keeps only its offset and size. */
template <class Member>
consteval FieldSpec make_field(std::size_t offset, const char* name, std::uint8_t flags) {
    static_assert(std::is_unsigned_v<Member>, "A node property must be an unsigned integer!");
    static_assert(sizeof(Member) == 1 || sizeof(Member) == 2 || sizeof(Member) == 4 ||
                  sizeof(Member) == 8);
    return FieldSpec{static_cast<std::uint16_t>(offset), sizeof(Member), name, flags};
}

template <class NodeProperties>
struct NodeSpec;

} // namespace goalx