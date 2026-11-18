#pragma once

#include "goalx/GoalxRequirements.hpp"

#include <cstdint>

#define GOALX_FIELD_SPEC(Node, member, name, flags)                                                \
    goalx::make_field<decltype(Node::member)>(offsetof(Node, member), name, flags)

/*
 * The node properties LogGOPSim expects from a trace.
 * These are what the simulator issues a decoder op with.
 */

struct SimulatorNodeProperties {
    std::uint64_t size;
    std::uint32_t peer, tag;
    std::uint8_t type, cpu, nic;
};

template <>
struct goalx::NodeSpec<SimulatorNodeProperties> {
    static constexpr std::array fields = {
        GOALX_FIELD_SPEC(SimulatorNodeProperties, type, "type", goalx::required),
        GOALX_FIELD_SPEC(SimulatorNodeProperties, peer, "peer", goalx::required),
        GOALX_FIELD_SPEC(SimulatorNodeProperties, size, "size", goalx::required),
        GOALX_FIELD_SPEC(SimulatorNodeProperties, tag, "tag", goalx::required),
        GOALX_FIELD_SPEC(SimulatorNodeProperties, cpu, "cpu", goalx::optional),
        GOALX_FIELD_SPEC(SimulatorNodeProperties, nic, "nic", goalx::optional),
    };
};