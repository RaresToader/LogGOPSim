#pragma once

namespace goalx {

template <typename NodeProperties, typename FieldType>
struct FieldSpec {
    FieldType NodeProperties::* member;
    const char* name;
};

template <typename NodeProperties, typename FieldType>
constexpr FieldSpec<NodeProperties, FieldType> field_spec(FieldType NodeProperties::* member,
                                                          const char* name) {
    return FieldSpec<NodeProperties, FieldType>{member, name};
}

template <class NodeProperties>
struct NodeSpec;

} // namespace goalx