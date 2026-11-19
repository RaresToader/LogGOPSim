#pragma once

#include "goalx/GoalxDecoder.hpp"
#include "goalx/GoalxFileManager.hpp"
#include "goalx/GoalxUtils.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <inttypes.h>
#include <map>
#include <print>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace goalx {

template <Decoder D>
class SerializedGraph {
  public:
    using NodeProperties = typename D::node_type;
    struct IdentifiableNode : NodeProperties {
        std::uint32_t offset;
        std::uint32_t rank;
    };

    SerializedGraph(const std::byte* rank_start, std::uint32_t rank, const Schema& schema,
                    const D& decoder);

    template <typename OutputNode>
        requires(std::is_trivially_copyable_v<OutputNode> &&
                 std::is_base_of_v<IdentifiableNode, OutputNode>)
    void GetExecutableNodes(std::vector<OutputNode>& out);

    void MarkNodeAsStarted(uint32_t offset);

    void MarkNodeAsDone(uint32_t offset);

    /* Get the number of nodes in the graph. */
    std::uint32_t num_nodes() const { return num_nodes_; }

    /* Produces a dot representation of the graph. This is useful for debugging purposes. */
    void write_as_dot(std::ostream& out) const;

  private:
    struct DeserializedNode : NodeProperties {
        std::vector<std::uint32_t> depend_on_me;
        std::vector<std::uint32_t> start_depend_on_me;
        std::uint32_t dependencies_count;
        std::uint32_t offset;
    };

    const std::byte* records_start_;
    const std::byte* appendix_start_;
    std::uint32_t num_root_nodes_;
    std::uint32_t num_nodes_;
    std::uint32_t my_rank_;
    std::uint16_t record_bytes_; // Bytes for each node record.
    std::uint8_t edge_bytes_;    // Bytes for each edge.
    std::vector<DeserializedNode> executable_nodes_;
    const D& decoder_;

    /* 0 = untouched; otherwise = outstanding dependencies + 1. */
    std::vector<std::uint32_t> remaining_dependencies_;

    /* Decrements each node's dependency counter from the EdgeRange,
     * and queues it into the ExecutableNodes when no dependencies remain. */
    void release_dependencies(EdgeRange edges);

    DeserializedNode get_node_by_offset(std::uint32_t offset) const;
};

template <Decoder D>
SerializedGraph<D>::SerializedGraph(const std::byte* rank_start, std::uint32_t rank,
                                    const Schema& schema, const D& decoder)
    : my_rank_{rank}, record_bytes_{schema.record_bytes}, edge_bytes_{schema.edge_bytes},
      decoder_{decoder} {
    std::size_t pos = 0;
    get(rank_start, pos, num_nodes_);
    get(rank_start, pos, num_root_nodes_);
    const std::byte* roots_nodes_start = rank_start + pos;
    records_start_ = roots_nodes_start + std::uint64_t{num_root_nodes_} * edge_bytes_;
    appendix_start_ = records_start_ + std::uint64_t{num_nodes_} * record_bytes_;

    remaining_dependencies_.resize(num_nodes_, 0);

    // Add the root nodes as the first executable nodes.
    for (std::uint32_t cnt = 0; cnt < num_root_nodes_; cnt++) {
        std::uint32_t offset = 0;
        std::memcpy(&offset, roots_nodes_start + std::uint64_t{cnt} * edge_bytes_, edge_bytes_);
        executable_nodes_.emplace_back(get_node_by_offset(offset));
    }
}

template <Decoder D>
typename SerializedGraph<D>::DeserializedNode
SerializedGraph<D>::get_node_by_offset(std::uint32_t offset) const {
    assert(offset < num_nodes_ && "Out of bounds node offset within a rank!");

    const std::byte* record = records_start_ + std::size_t{offset} * record_bytes_;

    DeserializedNode node{};
    decoder_.read_node(record, static_cast<NodeProperties&>(node));
    node.offset = offset;
    node.dependencies_count = decoder_.depcnt(record);

    auto read_list = [this](goalx::EdgeRange edges, std::vector<std::uint32_t>& out) {
        out.reserve(edges.count);
        for (std::uint32_t i = 0; i < edges.count; ++i) {
            std::uint32_t dependent = 0;
            std::memcpy(&dependent,
                        appendix_start_ + (edges.first + std::uint64_t{i}) * edge_bytes_,
                        edge_bytes_);
            out.push_back(dependent);
        }
    };

    read_list(decoder_.done_edges(record), node.depend_on_me);
    read_list(decoder_.start_edges(record), node.start_depend_on_me);

    return node;
}

template <Decoder D>
template <typename OutputNode>
    requires(std::is_trivially_copyable_v<OutputNode> &&
             std::is_base_of_v<typename SerializedGraph<D>::IdentifiableNode, OutputNode>)
void SerializedGraph<D>::GetExecutableNodes(std::vector<OutputNode>& out) {
    out.reserve(executable_nodes_.size());
    for (const auto& source : executable_nodes_) {
        out.emplace_back(
            IdentifiableNode{static_cast<const NodeProperties&>(source), source.offset, my_rank_});
    }
    executable_nodes_.clear();
}

template <Decoder D>
void SerializedGraph<D>::MarkNodeAsStarted(uint32_t offset) {
    assert(offset < num_nodes_ && "Out of bounds node offset within a rank!");

    const std::byte* record = records_start_ + std::size_t{offset} * record_bytes_;
    release_dependencies(decoder_.start_edges(record));
}

template <Decoder D>
void SerializedGraph<D>::MarkNodeAsDone(uint32_t offset) {
    assert(offset < num_nodes_ && "Out of bounds node offset within a rank!");

    const std::byte* record = records_start_ + std::size_t{offset} * record_bytes_;
    release_dependencies(decoder_.done_edges(record));
}

template <Decoder D>
void SerializedGraph<D>::release_dependencies(EdgeRange edges) {
    for (std::uint32_t i = 0; i < edges.count; ++i) {
        std::uint32_t dependent = 0;
        std::memcpy(&dependent, appendix_start_ + (edges.first + std::uint64_t{i}) * edge_bytes_,
                    edge_bytes_);
        auto& remaining = remaining_dependencies_[dependent];

        if (remaining == 0) {
            const std::byte* dependent_record =
                records_start_ + std::size_t{dependent} * record_bytes_;
            remaining = decoder_.depcnt(dependent_record) + 1;
        }

        assert(remaining > 1 && "Releasing too many dependencies for the node!");

        if (--remaining == 1) {
            auto& node = executable_nodes_.emplace_back(get_node_by_offset(dependent));
            node.dependencies_count = 0;
        }
    }
}

template <Decoder D>
void SerializedGraph<D>::write_as_dot(std::ostream& out) const {
    out << "digraph rank" << my_rank_ << " {\n";
    for (std::uint32_t offset = 0; offset < num_nodes_; ++offset) {
        const auto node = get_node_by_offset(offset);
        out << std::format("  n{} [label=\"{}\"];\n", offset, offset);
        for (const auto dependent : node.depend_on_me) {
            out << std::format("  n{} -> n{};\n", offset, dependent);
        }
        for (const auto dependent : node.start_depend_on_me) {
            out << std::format("  n{} -> n{} [style=dashed, label=\"start\"];\n", offset,
                               dependent);
        }
    }
    out << "}\n";
}

} // namespace goalx