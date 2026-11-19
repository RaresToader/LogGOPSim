#pragma once

#include "goalx/GoalxParser.hpp"
#include "goalx/GoalxRequirements.hpp"

#include <cstdint>

#define STRICT_ORDER // this is needed to keep order between Send/Recv and LocalOps in NBC case :-/
#define LIST_MATCH   // enables debugging the queues (check if empty)
#define HOSTSYNC     // this is experimental to count synchronization times induced by message
                     // transmissions
#define GOALX_FIELD_SPEC(Node, member, name, flags)                                                \
    goalx::make_field<decltype(Node::member)>(offsetof(Node, member), name, flags)

/*
 * The node properties LogGOPSim expects from a trace.
 * These are what the simulator issues a decoder op with.
 *
 * Note: Experiment with packing of the node structure,
 * that could save us up to 30% in space, but might harm time.
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

using btime_t = uint64_t;
// TODO: Because of extending from the base class, fields are no longer ordered by size.
struct SimulatorNode : SerializedGraph<SimulatorNodeProperties>::IdentifiableNode {
    btime_t time;
    btime_t starttime;
#ifdef HOSTSYNC
    btime_t syncstart;
#endif
#ifdef STRICT_ORDER
    btime_t ts; /* this is a timestamp that determines the (original) insertion order of
                  elements in the queue, it is increased for every new element, not for
                  re-insertions! Needed for correctness. */
#endif
    uint32_t handle; // handle for network layer :-/} int main(int argc, char** argv) {
};

struct gnp_op_comp_func {
    bool operator()(const SimulatorNode& x, const SimulatorNode& y) const {
        return x.type < y.type;
    }
};

/* This is a comparison functor that can be used to compare and sort
 * by time. */
struct aqcompare_func {
    bool operator()(const SimulatorNode& x, const SimulatorNode& y) const {
        if (x.time > y.time) return true;
#ifdef STRICT_ORDER
        if (x.time == y.time && x.ts > y.ts) return true;
#endif
        return false;
    }
};

// mnemonic defines for op type
inline constexpr int OP_SEND = 1;
inline constexpr int OP_RECV = 2;
inline constexpr int OP_LOCOP = 3;
inline constexpr int OP_MSG = 4;

// wildcards for the peer and tag of a receive
inline constexpr std::uint32_t ANY_SOURCE = ~std::uint32_t{0};
inline constexpr std::uint32_t ANY_TAG = ~std::uint32_t{0};