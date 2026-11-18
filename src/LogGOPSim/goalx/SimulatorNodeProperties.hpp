#pragma once

#include "goalx/GoalxRequirements.hpp"

#include <cstdint>
#include <tuple>

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
    static constexpr auto fields =
        std::make_tuple(goalx::field_spec(&SimulatorNodeProperties::type, "type"),
                        goalx::field_spec(&SimulatorNodeProperties::peer, "peer"),
                        goalx::field_spec(&SimulatorNodeProperties::size, "size"),
                        goalx::field_spec(&SimulatorNodeProperties::tag, "tag"),
                        goalx::field_spec(&SimulatorNodeProperties::cpu, "cpu"),
                        goalx::field_spec(&SimulatorNodeProperties::nic, "nic"));
};