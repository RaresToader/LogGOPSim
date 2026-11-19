#pragma once

#include "goalx/GoalxDecoder.hpp"
#include "goalx/GoalxFileManager.hpp"
#include "goalx/GoalxSerializedGraph.hpp"
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
class Parser {
  public:
    using Schedules = std::vector<goalx::SerializedGraph<D>>;

    explicit Parser(const GoalxFileManager& file_manager, const D& decoder);

    const Schedules& schedules() const { return schedules_; }
    Schedules& schedules() { return schedules_; }

    template <typename OutputNode>
    void GetExecutableNodes(std::vector<OutputNode>& out, std::uint32_t rank) {
        return schedules_[rank].template GetExecutableNodes<OutputNode>(out);
    }

    void MarkNodeAsDone(std::uint32_t rank, std::uint32_t offset) {
        return schedules_[rank].MarkNodeAsDone(offset);
    }

    void MarkNodeAsStarted(std::uint32_t rank, std::uint32_t offset) {
        return schedules_[rank].MarkNodeAsStarted(offset);
    }

  private:
    Schedules schedules_;
    const GoalxFileManager& file_manager_;
    D decoder_;
};

template <Decoder D>
Parser<D>::Parser(const GoalxFileManager& file_manager, const D& decoder)
    : file_manager_{file_manager}, decoder_{decoder} {
    schedules_.reserve(file_manager_.num_ranks());
    std::size_t pos = 0;
    for (uint32_t cnt = 0; cnt < file_manager_.num_ranks(); cnt++) {
        std::uint64_t rank_start_offset;
        std::uint64_t rank_end_offset;
        get(file_manager_.jump_table_start(), pos, rank_start_offset);
        get(file_manager_.jump_table_start(), pos, rank_end_offset);
        schedules_.emplace_back(SerializedGraph{file_manager_.mapping_start() + rank_start_offset,
                                                cnt, file_manager_.schema(), decoder_});
    }
}

} // namespace goalx