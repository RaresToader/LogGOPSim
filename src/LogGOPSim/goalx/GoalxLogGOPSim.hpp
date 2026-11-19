// TODO: Currently only the code which assumes LIST_MATCH is ported. Maybe port the hashmap variant
// as well?
#include "Network.hpp"
#include "Noise.hpp"
#include "TimelineVisualization.hpp"
#include "goalx/GoalxFileManager.hpp"
#include "goalx/GoalxGenericDecoder.hpp"
#include "goalx/GoalxRequirements.hpp"
#include "goalx/GoalxTimedComputation.hpp"
#include "goalx/SimulatorNodeProperties.hpp"
#include "loggopsim_cmdline.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <list>
#include <print>
#include <queue>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

// build_info.h is written by the Makefile; a CMake build passes the same macros on the command line
#if !defined(GOALX_BUILD_COMMIT) && defined(__has_include)
#if __has_include("build_info.h")
#include "build_info.h"
#endif
#endif
#ifndef GOALX_BUILD_COMMIT
#define GOALX_BUILD_COMMIT "unknown"
#endif
#ifndef GOALX_BUILD_FLAGS
#define GOALX_BUILD_FLAGS "unknown"
#endif

#if defined(__GNUC__)
#define MATCH_INLINE inline __attribute__((always_inline))
#else
#define MATCH_INLINE inline
#endif

using Node = SimulatorNode;

struct ruqelem_t {
    btime_t starttime; // only for visualization
    uint32_t size, src, tag, offset;
};
using ruq_t = std::list<ruqelem_t>;

static MATCH_INLINE int match(const Node& elem, ruq_t& q, ruqelem_t& retelem, bool print) {
    int match_attempts = 0;

    if (print)
        std::println("++ [{}] searching matching queue for src {} tag {}", elem.rank, elem.peer,
                     elem.tag);
    for (auto iter = q.begin(); iter != q.end(); ++iter) {
        match_attempts++;
        if (elem.peer == ANY_SOURCE || iter->src == ANY_SOURCE || iter->src == elem.peer) {
            if (elem.tag == ANY_TAG || iter->tag == ANY_TAG || iter->tag == elem.tag) {
                retelem = *iter;
                q.erase(iter);
                return match_attempts;
            }
        }
    }
    return -1;
}

/* JSON string literal */
static std::string json_str(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

/* one JSON object with this run's measurements and the build that produced them (for audit and
 * plots) */
static void emit_results(const char* path, const char* trace, const Schema& schema,
                         const char* build, double sim_s, std::uint64_t events,
                         std::uint64_t max_finish) {
    struct stat st;
    const long long bytes = stat(trace, &st) == 0 ? static_cast<long long>(st.st_size) : -1;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    const unsigned long long rss = ru.ru_maxrss; // bytes
#else
    const unsigned long long rss = ru.ru_maxrss * 1024ULL; // KiB
#endif
    FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        std::perror(path);
        std::exit(EXIT_FAILURE);
    }
    std::println(f, "{{");
    std::println(f, "  \"trace\": {},", json_str(trace));
    std::println(f, "  \"trace_bytes\": {},", bytes);
    std::println(f, "  \"format\": {},", json_str("goalx-1.1"));
    std::println(f, "  \"schema_hash\": {},", json_str(std::format("{:016x}", schema.hash())));
    std::println(f, "  \"decoder\": {},", json_str("generic"));
    std::println(f, "  \"build\": {},", json_str(build));
    std::println(f, "  \"compiler\": {},", json_str(__VERSION__));
    std::println(f, "  \"cxxflags\": {},", json_str(GOALX_BUILD_FLAGS));
    std::println(f, "  \"git_commit\": {},", json_str(GOALX_BUILD_COMMIT));
    // fprintf(f, "  \"header_s\": %.9f,\n", parser.timing.header_s);
    // std::println(f, "  \"validate_s\": {:.9f},", 0.0); // part of decoder_init_s here
    //   fprintf(f, "  \"decoder_init_s\": %.9f,\n", parser.timing.decoder_init_s);
    //   fprintf(f, "  \"counters_s\": %.9f,\n", parser.timing.counters_s);
    std::println(f, "  \"sim_s\": {:.9f},", sim_s);
    std::println(f, "  \"events\": {},", events);
    std::println(f, "  \"peak_rss_bytes\": {},", rss);
    std::println(f, "  \"max_finish_time\": {}", max_finish);
    std::println(f, "}}");
    std::fclose(f);
}

int main(int argc, char** argv) {

#ifdef STRICT_ORDER
    btime_t aqtime = 0;
#endif

    gengetopt_args_info args_info;
    if (cmdline_parser(argc, argv, &args_info) != 0) {
        std::println(stderr, "Couldn't parse command line arguments!");
        std::exit(EXIT_FAILURE);
    }

#ifndef LIST_MATCH
    if (args_info.qstat_given) {
        std::println(
            "WARNING: --qstat option provided, but LogGOPSim was compiled with LIST_MATCH;\n"
            "         statistics on match queue behavior are NOT valid.\n");
    }
#endif

    // TODO: these could probably live in a struct
    const int o = args_info.LogGOPS_o_arg;
    const int O = args_info.LogGOPS_O_arg;
    const int g = args_info.LogGOPS_g_arg;
    const int L = args_info.LogGOPS_L_arg;
    const int G = args_info.LogGOPS_G_arg;
    const bool print = args_info.verbose_given;
    const uint32_t S = args_info.LogGOPS_S_arg;

#ifdef __OPTIMIZE__
    const char* build = "optimized";
#else
    const char* build = "debug";
#endif

    auto [file_manager, time_create_schema] =
        goalx::timed([&args_info] { return goalx::GoalxFileManager{args_info.filename_arg}; });
    GoalxParser parser{file_manager};

    const uint32_t num_ranks = file_manager.num_ranks();
    // The largest value of a field in the trace, 0 if the trace doesn't have it.
    const auto field_max = [&file_manager](const char* name) -> uint32_t {
        const FieldDesc* field = file_manager.schema().find(name);
        return field ? static_cast<uint32_t>(field->max_value) : 0;
    };
    const uint32_t max_cpu = field_max("cpu");
    const uint32_t max_nic = field_max("nic");
    const uint32_t ncpus = max_cpu + 1;
    const uint32_t nnics = max_nic + 1;

    if (args_info.print_schema_given) {
        file_manager.schema().print(stdout, num_ranks, max_cpu, max_nic);
        return 0;
    }

    std::println("size: {} ({} CPUs, {} NICs); L={}, o={} g={}, G={}, O={}, P={}, S={}", num_ranks,
                 ncpus, nnics, L, o, g, G, O, num_ranks, S);

    // TODO: decoder throughput only bench

    Network net(&args_info);
    TimelineVisualization tlviz(&args_info, num_ranks);
    Noise osnoise(&args_info, num_ranks);

    // TODO: Clean these. They only seem to be used is `qstat_given` is set to true, so only for
    // stats
    std::vector<int> rq_max(0);
    std::vector<int> uq_max(0);
    std::vector<std::vector<std::pair<int, btime_t>>> rq_matches(0);
    std::vector<std::vector<std::pair<int, btime_t>>> uq_matches(0);
    std::vector<std::vector<std::pair<int, btime_t>>> rq_misses(0);
    std::vector<std::vector<std::pair<int, btime_t>>> uq_misses(0);
    std::vector<std::vector<btime_t>> rq_times(0);
    std::vector<std::vector<btime_t>> uq_times(0);
    if (args_info.qstat_given) {
        rq_max.resize(num_ranks);
        uq_max.resize(num_ranks);
        for (int i : rq_max) {
            rq_max[i] = 0;
            uq_max[i] = 0;
        }
        rq_matches.resize(num_ranks);
        uq_matches.resize(num_ranks);
        rq_misses.resize(num_ranks);
        uq_misses.resize(num_ranks);
        rq_times.resize(num_ranks);
        uq_times.resize(num_ranks);
    }

    // TODO: find a better way of specifying the comparator
    std::priority_queue<Node, std::vector<Node>, aqcompare_func> active_queue;
    std::vector<ruq_t> rq(num_ranks), uq(num_ranks); // receive queue, unexpected queue

    // next available time for o, g(receive) and g(send)
    std::vector<std::vector<btime_t>> nexto(num_ranks), nextgr(num_ranks), nextgs(num_ranks);
    // initialize o and g for all PEs and hosts
    for (uint32_t i = 0; i < num_ranks; i++) {
        nexto[i].resize(ncpus);
        std::fill(nexto[i].begin(), nexto[i].end(), 0);
        nextgr[i].resize(nnics);
        std::fill(nextgr[i].begin(), nextgr[i].end(), 0);
        nextgs[i].resize(nnics);
        std::fill(nextgs[i].begin(), nextgs[i].end(), 0);
    }
#ifdef HOSTSYNC
    std::vector<btime_t> hostsync(num_ranks);
#endif

    // TODO: Maybe reuse the timed function abstraction for the simulation
    std::chrono::steady_clock::time_point tsim = std::chrono::steady_clock::now();

    // TODO: probably an API like this is needed
    // uint64_t num_events = parser.NumNodes();
    std::vector<Node> free_ops;
    for (uint32_t rank = 0; rank < num_ranks; rank++) {
        parser.GetExecutableNodes<Node>(free_ops,
                                        rank); // appends to free_ops, which is cleared after use
        std::sort(free_ops.begin(), free_ops.end(), gnp_op_comp_func());
        for (Node& node : free_ops) {
            node.time = 0;
#ifdef STRICT_ORDER
            node.ts = aqtime++;
#endif
            switch (node.type) {
            case OP_LOCOP:
                if (print)
                    std::println("init {} ({},{}) loclop: {}", rank, node.cpu, node.nic, node.size);
                break;
            case OP_SEND:
                if (print)
                    std::println("init {} ({},{}) send to: {}, tag: {}, size: {}", rank, node.cpu,
                                 node.nic, node.peer, node.tag, node.size);
                break;
            case OP_RECV:
                if (print)
                    std::println("init {} ({},{}) recvs from: {}, tag: {}, size: {}", rank,
                                 node.cpu, node.nic, node.peer, node.tag, node.size);
                break;
            default:
                std::println("not implemented!");
            }

            active_queue.push(node);
        }
        free_ops.clear();
    }

    // the hosts that actually finished something in this round -- a host is
    // added whenever an irequires or requires is satisfied
    std::vector<uint32_t> check_hosts;

    while (!active_queue.empty()) {
        Node elem = active_queue.top();
        active_queue.pop();
        check_hosts.clear();

        // the BIG switch on element type that we just found
        switch (elem.type) {
        case OP_LOCOP: {
            if (print)
                std::println("[{}] found loclop of length {} - t: {} (CPU: {})", elem.rank,
                             elem.size, elem.time, elem.cpu);
            if (nexto[elem.rank][elem.cpu] <= elem.time) {
                // check if OS Noise occurred
                btime_t noise = osnoise.get_noise(elem.rank, elem.time, elem.time + elem.size);
                nexto[elem.rank][elem.cpu] = elem.time + elem.size + noise;
                // satisfy irequires and requires
                parser.MarkNodeAsStarted(elem);
                parser.MarkNodeAsDone(elem);
                check_hosts.push_back(elem.rank);
                // add to timeline
                tlviz.add_loclop(elem.rank, elem.time, elem.time + elem.size, elem.cpu);
            } else {
                if (print) std::println("-- locop local o not available -- reinserting");
                elem.time = nexto[elem.rank][elem.cpu];
                active_queue.push(elem);
            }
        } break;

        case OP_SEND: { // a send op
            if (print)
                std::println("[{}] found send to {} - t: {} (CPU: {})", elem.rank, elem.peer,
                             elem.time, elem.cpu);

            if (std::max(nexto[elem.rank][elem.cpu], nextgs[elem.rank][elem.nic]) <=
                elem.time) { // local o,g available!
                if (print) std::println("-- satisfy local irequires");
                parser.MarkNodeAsStarted(elem);
                check_hosts.push_back(elem.rank);

                // check if OS Noise occurred
                btime_t noise = osnoise.get_noise(elem.rank, elem.time, elem.time + o);
                nexto[elem.rank][elem.cpu] = elem.time + o + (elem.size - 1) * O + noise;
                nextgs[elem.rank][elem.nic] =
                    elem.time + g +
                    (elem.size - 1) * G; // TODO: G should be charged in network layer only
                tlviz.add_osend(elem.rank, elem.time, elem.time + o + (elem.size - 1) * O,
                                elem.cpu);
                tlviz.add_noise(elem.rank, elem.time + o + (elem.size - 1) * O,
                                elem.time + o + (elem.size - 1) * O + noise, elem.cpu);

                // insert packet into network layer
                net.insert(elem.time, elem.rank, elem.peer, elem.size, &elem.handle);

                // From here on elem is the message: it belongs to the receiver and its peer is the
                // sender. `send` still identifies the send node for the parser.
                // TODO: Is the copy necessary here?
                const Node send = elem;
                elem.type = OP_MSG;
                elem.rank = send.peer;
                elem.peer = send.rank;
                elem.starttime = elem.time;
                elem.time = elem.time + o +
                            L; // arrival time of first byte only (G will be charged at receiver)
#ifdef STRICT_ORDER
                // TODO: num_events++ (MSG is a new event), once --progress is ported
                elem.ts = aqtime++; // new element in queue
#endif
                if (print)
                    std::println("-- [{}] send inserting msg to {}, t: {}", send.rank, elem.rank,
                                 elem.time);
                active_queue.push(elem);

                if (elem.size <= S) { // eager message
                    if (print)
                        std::println("-- [{}] eager -- satisfy local requires at t: {}", send.rank,
                                     elem.starttime + o);
                    // satisfy local requires
                    parser.MarkNodeAsDone(send);
                } else {
                    if (print)
                        std::println("-- [{}] start rendezvous message to: {} (offset: {})",
                                     send.rank, elem.rank, elem.offset);
                }
            } else { // local o,g unavailable - retry later
                if (print) std::println("-- send local o,g not available -- reinserting");
                elem.time = std::max(nexto[elem.rank][elem.cpu], nextgs[elem.rank][elem.nic]);
                active_queue.push(elem);
            }
        } break;

        case OP_RECV: {
            if (print)
                std::println("[{}] found recv from {} - t: {} (CPU: {})", elem.rank, elem.peer,
                             elem.time, elem.cpu);

            parser.MarkNodeAsStarted(elem);
            check_hosts.push_back(elem.rank);
            if (print) std::println("-- satisfy local irequires");

            // UPDATE the maximum UQ size
            if (args_info.qstat_given) {
                uq_max[elem.rank] =
                    std::max(static_cast<int>(uq[elem.rank].size()), uq_max[elem.rank]);
            }

            ruqelem_t matched_elem;
            // NUMBER of elements that were searched during message matching
            const int match_attempts = match(elem, uq[elem.rank], matched_elem, print);
            if (match_attempts >= 0) { // found it in local UQ
                if (print) std::println("-- found in local UQ");
                if (args_info.qstat_given) {
                    // RECORD match queue statistics
                    uq_matches[elem.rank].emplace_back(match_attempts, elem.time);
                    uq_times[elem.rank].push_back(elem.time - matched_elem.starttime);
                }

                if (elem.size > S) { // rendezvous - free remote request
                    // satisfy remote requires
                    parser.MarkNodeAsDone(
                        matched_elem.src,
                        matched_elem.offset); // this must be the offset of the remote packet!
                    check_hosts.push_back(matched_elem.src);

                    // TODO: uh, this is ugly ... we actually need to set the
                    // time of the remote freed elements to elem.time now :-/
                    // however, there is no simple way to do this :-(
                    // but since we just got the elem with elem.time from the
                    // queue, we can safely set the remote clocks to elem.time
                    // (there is no event < elem.time in the queue) this
                    // manipulation is dangerous, think before you change
                    // anything!
                    // TODO: do we need to add some latency for the ACK to
                    // travel??? -- this can *not* be added here safely!
                    if (nexto[matched_elem.src][elem.cpu] < elem.time)
                        nexto[matched_elem.src][elem.cpu] = elem.time;
                    if (nextgs[matched_elem.src][elem.nic] < elem.time)
                        nextgs[matched_elem.src][elem.nic] = elem.time;

                    if (print)
                        std::println("-- satisfy remote requires at host {} (offset: {})",
                                     elem.peer, elem.offset);
                    tlviz.add_transmission(elem.rank, matched_elem.src, matched_elem.starttime + o,
                                           elem.time, elem.size, G);
                }
                tlviz.add_orecv(elem.rank, elem.time, elem.time + o, elem.cpu);

                // satisfy local requires
                parser.MarkNodeAsDone(elem);
                check_hosts.push_back(elem.rank);
                if (print) std::println("-- satisfy local requires");
            } else { // not found in local UQ - add to RQ
                if (args_info.qstat_given) {
                    // RECORD match queue statistics
                    uq_misses[elem.rank].emplace_back(static_cast<int>(uq[elem.rank].size()),
                                                      elem.time);
                }
                if (print) std::println("-- not found in local UQ -- add to RQ");
                ruqelem_t nelem{};
                nelem.size = elem.size;
                nelem.src = elem.peer;
                nelem.tag = elem.tag;
                nelem.offset = elem.offset;
#ifdef LIST_MATCH
                rq[elem.rank].push_back(nelem);
#else
                // TODO: hash map version
#endif
            }
        } break;

        case OP_MSG: {
            if (print)
                std::println("[{}] found msg from {}, t: {} (CPU: {})", elem.rank, elem.peer,
                             elem.time, elem.cpu);
            const uint64_t earliestfinish =
                net.query(elem.starttime, elem.time, elem.peer, elem.rank, elem.size, &elem.handle);
            if (earliestfinish <= elem.time /* msg made it through network */ &&
                std::max(nexto[elem.rank][elem.cpu], nextgr[elem.rank][elem.nic]) <=
                    elem.time /* local o,g available! */) {
                if (print)
                    std::println("-- msg o,g available (nexto: {}, nextgr: {})",
                                 nexto[elem.rank][elem.cpu], nextgr[elem.rank][elem.nic]);
                // check if OS Noise occurred
                btime_t noise = osnoise.get_noise(elem.rank, elem.time, elem.time + o);
                nexto[elem.rank][elem.cpu] =
                    elem.time + o + noise +
                    std::max(
                        (elem.size - 1) * O,
                        (elem.size - 1) *
                            G) /* message is only received after G is charged !! TODO: consuming o
                                  seems a bit odd in the LogGP model but well in practice */
                    ;
                nextgr[elem.rank][elem.nic] = elem.time + g + (elem.size - 1) * G;

                // UPDATE the maximum RQ size
                if (args_info.qstat_given) {
                    rq_max[elem.rank] =
                        std::max(static_cast<int>(rq[elem.rank].size()), rq_max[elem.rank]);
                }

                ruqelem_t matched_elem;
                // NUMBER of elements that were searched during message matching
                const int match_attempts = match(elem, rq[elem.rank], matched_elem, print);
                if (match_attempts >= 0) { // found it in RQ
                    if (args_info.qstat_given) {
                        // RECORD match queue statistics
                        rq_matches[elem.rank].emplace_back(match_attempts, elem.time);
                        /* Amount of time spent in queue */
                        rq_times[elem.rank].push_back(elem.time - matched_elem.starttime);
                    }

                    if (print) std::println("-- found in RQ");
                    if (elem.size > S) { // rendezvous - free remote request
                        // satisfy remote requires: the send node on the sender
                        parser.MarkNodeAsDone(elem.peer, elem.offset);
                        check_hosts.push_back(elem.peer);
                        // TODO: uh, this is ugly ... we actually need to set the
                        // time of the remote freed elements to elem.time now :-/
                        // however, there is no simple way to do this :-(
                        // but since we just got the elem with elem.time from the
                        // queue, we can safely set the remote clocks to elem.time
                        // (there is no event < elem.time in the queue) this
                        // manipulation is dangerous, think before you change
                        // anything!
                        // TODO: do we need to add some latency for the ACK to
                        // travel??? -- this can *not* be added here safely!
                        if (nexto[elem.peer][elem.cpu] < elem.time)
                            nexto[elem.peer][elem.cpu] = elem.time;
                        if (nextgs[elem.peer][elem.nic] < elem.time)
                            nextgs[elem.peer][elem.nic] = elem.time;

                        if (print) std::println("-- satisfy remote requires on host {}", elem.peer);
                    }
                    tlviz.add_transmission(elem.peer, elem.rank, elem.starttime + o, elem.time,
                                           elem.size, G);
                    tlviz.add_orecv(
                        elem.rank, elem.time + (elem.size - 1) * G - (elem.size - 1) * O,
                        elem.time + o + std::max((elem.size - 1) * O, (elem.size - 1) * G),
                        elem.cpu);
                    tlviz.add_noise(
                        elem.rank,
                        elem.time + o + std::max((elem.size - 1) * O, (elem.size - 1) * G),
                        elem.time + o + noise + std::max((elem.size - 1) * O, (elem.size - 1) * G),
                        elem.cpu);
                    // satisfy local requires: the matched receive node on this rank
                    parser.MarkNodeAsDone(elem.rank, matched_elem.offset);
                    check_hosts.push_back(elem.rank);
                } else { // not in RQ
                    if (args_info.qstat_given) {
                        // RECORD match queue statistics
                        rq_misses[elem.rank].emplace_back(static_cast<int>(rq[elem.rank].size()),
                                                          elem.time);
                    }

                    if (print) std::println("-- not found in RQ - add to UQ");
                    ruqelem_t nelem{};
                    nelem.size = elem.size;
                    nelem.src = elem.peer;
                    nelem.tag = elem.tag;
                    nelem.offset = elem.offset;
                    nelem.starttime = elem.time; // when it was started
#ifdef LIST_MATCH
                    uq[elem.rank].push_back(nelem);
#else
                    // TODO: hash map version
#endif
                }
            } else {
                elem.time =
                    std::max(std::max(nexto[elem.rank][elem.cpu], nextgr[elem.rank][elem.nic]),
                             earliestfinish);
                if (print) std::println("-- msg o,g not available -- reinserting");
                active_queue.push(elem);
            }
        } break;

        default:
            std::println("not supported");
            break;
        }

        // do only ask hosts that actually completed something in this round!
        std::sort(check_hosts.begin(), check_hosts.end());
        check_hosts.erase(std::unique(check_hosts.begin(), check_hosts.end()), check_hosts.end());

        for (uint32_t rank : check_hosts) {
            parser.GetExecutableNodes<Node>(free_ops, rank);
            // ensure that the free ops are ordered by type
            std::sort(free_ops.begin(), free_ops.end(), gnp_op_comp_func());

            // walk all new free operations and throw them in the queue
            for (Node& node : free_ops) {
#ifdef STRICT_ORDER
                node.ts = aqtime++;
#endif
                switch (node.type) {
                case OP_LOCOP:
                    node.time = nexto[rank][node.cpu];
                    if (print)
                        std::println("{} ({},{}) loclop: {}, time: {}, offset: {}", rank, node.cpu,
                                     node.nic, node.size, node.time, node.offset);
                    break;
                case OP_SEND:
                    node.time = std::max(nexto[rank][node.cpu], nextgs[rank][node.nic]);
                    if (print)
                        std::println(
                            "{} ({},{}) send to: {}, tag: {}, size: {}, time: {}, offset: {}", rank,
                            node.cpu, node.nic, node.peer, node.tag, node.size, node.time,
                            node.offset);
                    break;
                case OP_RECV:
                    node.time = nexto[rank][node.cpu];
                    if (print)
                        std::println(
                            "{} ({},{}) recvs from: {}, tag: {}, size: {}, time: {}, offset: {}",
                            rank, node.cpu, node.nic, node.peer, node.tag, node.size, node.time,
                            node.offset);
                    break;
                default:
                    std::println("not implemented!");
                }

                active_queue.push(node);
            }
            free_ops.clear();
        }

        // TODO: --progress (needs the total number of nodes, see num_events above)
    }

    const double sim_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tsim).count();

#ifndef STRICT_ORDER
    btime_t aqtime = 0;
#endif
    // a rank finishes when the last of its CPUs becomes free
    const auto finish_time = [&nexto](uint32_t rank) {
        return *std::max_element(nexto[rank].begin(), nexto[rank].end());
    };

    std::println("PERFORMANCE: Processes: {} \t Events: {} \t Time: {:.6f} s \t Speed: {:.2f} ev/s",
                 num_ranks, aqtime, sim_s, static_cast<double>(aqtime) / sim_s);
    if (args_info.timing_given) {
        // TODO: this hardcores the decoder name as "generic" for now. Also, some timing info is
        // missing.
        std::println("TIMING: decoder=generic build={} header_s={:.6f} sim_s={:.6f} events={}",
                     build, time_create_schema, sim_s, aqtime);
    }
    if (args_info.emit_results_given) {
        btime_t max_finish = 0;
        for (uint32_t i = 0; i < num_ranks; i++) max_finish = std::max(max_finish, finish_time(i));
        emit_results(args_info.emit_results_arg, args_info.filename_arg, file_manager.schema(),
                     build, sim_s, aqtime, max_finish);
    }

    // check if all queues are empty!!
    bool ok = true;
    for (uint32_t i = 0; i < num_ranks; i++) {
#ifdef LIST_MATCH
        if (!uq[i].empty()) {
            std::println("unexpected queue on host {} contains {} elements!", i, uq[i].size());
            for (const ruqelem_t& e : uq[i]) std::println(" src: {}, tag: {}", e.src, e.tag);
            ok = false;
        }
        if (!rq[i].empty()) {
            std::println("receive queue on host {} contains {} elements!", i, rq[i].size());
            for (const ruqelem_t& e : rq[i]) std::println(" src: {}, tag: {}", e.src, e.tag);
            ok = false;
        }
#endif
    }

    if (ok) {
        if (num_ranks <= 16 && !args_info.batchmode_given) { // print all hosts
            std::println("Times: ");
            for (uint32_t i = 0; i < num_ranks; i++) std::println("Host {}: {}", i, finish_time(i));
        } else { // print only maximum
            btime_t max = 0;
            uint32_t host = 0;
            for (uint32_t i = 0; i < num_ranks; i++) { // find maximum end time
                const btime_t cur = finish_time(i);
                if (cur > max) {
                    host = i;
                    max = cur;
                }
            }
            std::println("Maximum finishing time at host {}: {} ({:g} s)", host, max,
                         static_cast<double>(max) / 1e9);
        }

        // WRITE match queue statistics
        if (args_info.qstat_given) {
            // one line per rank: the maximum queue depth
            const auto write_max = [&args_info](const char* name,
                                                const std::vector<int>& per_rank) {
                const std::string filename = std::format("{}-{}", args_info.qstat_arg, name);
                std::ofstream file(filename);
                if (!file.is_open()) {
                    std::println(stderr, "Can't open {} data file ({})", name, filename);
                    return;
                }
                for (int n : per_rank) std::println(file, "{}", n);
            };
            // one line per rank: "<elements searched>,<time>" for each search
            const auto write_searches =
                [&args_info](const char* name,
                             const std::vector<std::vector<std::pair<int, btime_t>>>& per_rank) {
                    const std::string filename = std::format("{}-{}", args_info.qstat_arg, name);
                    std::ofstream file(filename);
                    if (!file.is_open()) {
                        std::println(stderr, "Can't open {} data file ({})", name, filename);
                        return;
                    }
                    for (const auto& searches : per_rank) {
                        for (const auto& [depth, time] : searches)
                            std::print(file, "{},{} ", depth, time);
                        std::println(file, "");
                    }
                };

            write_max("rq-max.data", rq_max); // maximum RQ depth
            write_max("uq-max.data", uq_max); // maximum UQ depth
            write_searches("rq-hit.data",
                           rq_matches); // RQ depth searched for each successful search
            write_searches("uq-hit.data",
                           uq_matches); // UQ depth searched for each successful search
            write_searches("rq-miss.data", rq_misses); // RQ length for each unsuccessful search
            write_searches("uq-miss.data", uq_misses); // UQ length for each unsuccessful search
        }
    }

    return 0;
}