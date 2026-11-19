/*
 * LogGOPSim on the new GOALX reader (GoalxFileManager + a goalx::Decoder + goalx::Parser).
 *
 * The simulation is the one of LogGOPSim.cpp, unchanged: same events, same order, same
 * times. Only the graph access differs: nodes come from goalx::SerializedGraph, and an
 * event is the decoded node (IdentifiableNode: the NodeProperties + offset + rank) plus the
 * simulator's own state. Compared with graph_node_properties: target -> peer,
 * host -> rank, proc -> cpu. The trace's op types equal OP_SEND/OP_RECV/OP_LOCOP; OP_MSG
 * is set by the simulator.
 *
 * Build from src/LogGOPSim (loggopsim_cmdline.c compiled as C):
 *
 *   gcc -O2 -c loggopsim_cmdline.c -o /tmp/loggopsim_cmdline.o
 *   g++ -std=c++23 -O2 -I. -x c++ goalx/GoalxLogGOPSim.hpp -x none /tmp/loggopsim_cmdline.o \
 *       -lcgraph -o ../../build/LogGOPSim-goalx
 */

// These change the event layout and the queues: they must come before every include.
#define STRICT_ORDER // this is needed to keep order between Send/Recv and LocalOps in NBC case :-/
#define LIST_MATCH   // enables debugging the queues (check if empty)
#define HOSTSYNC     // this is experimental to count synchronization times induced by message
                     // transmissions

// Network.hpp, Noise.hpp and TimelineVisualization.hpp use these without including them
#include "LogGOPSim.hpp" // btime_t, OP_*, ANY_SOURCE / ANY_TAG

#include "Network.hpp"
#include "Noise.hpp"
#include "TimelineVisualization.hpp"
#include "goalx/GoalxDecoder.hpp"
#include "goalx/GoalxFileManager.hpp"
#include "goalx/GoalxParser.hpp"
#include "goalx/GoalxSerializedGraph.hpp"
#include "goalx/SimulatorNodeProperties.hpp"
#include "goalx/decoders/GoalxGenericDecoder.hpp"
#include "loggopsim_cmdline.h"
#include "scratch/TimedComputation.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <list>
#include <print>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#ifndef LIST_MATCH
#error "GoalxLogGOPSim supports only LIST_MATCH matching"
#endif

namespace {

bool print = false; // -v

/* One entry of the active queue: a node of the graph plus the simulator's state for it. */
template <goalx::Decoder D>
struct Event : goalx::SerializedGraph<D>::IdentifiableNode { // properties, offset, rank
    btime_t time = 0;
    btime_t starttime = 0; // only used for MSGs to identify start times
    // No syncstart (graph_node_properties has it under HOSTSYNC, but nothing reads it):
    // without it an event is 64 bytes, one cache line, which the active queue moves around.
#ifdef STRICT_ORDER
    btime_t ts = 0; // insertion order, increased for every new element, not for re-insertions
#endif
    std::uint32_t handle = 0; // handle for the network layer
};

/* The active queue's order (aqcompare_func): earliest time first, then insertion order. */
struct LaterFirst {
    template <class E>
    bool operator()(const E& x, const E& y) const {
        if (x.time > y.time) return true;
#ifdef STRICT_ORDER
        if (x.time == y.time && x.ts > y.ts) return true;
#endif
        return false;
    }
};

/* The order of new executable nodes (gnp_op_comp_func). */
struct ByType {
    template <class E>
    bool operator()(const E& x, const E& y) const {
        return x.type < y.type;
    }
};

struct ruqelem_t {
    btime_t starttime; // only for visualization
    std::uint32_t size, src, tag, offset;
};
using ruq_t = std::list<ruqelem_t>;

/* Finds and removes the first matching element: the number of elements searched, -1 if none. */
template <class E>
inline __attribute__((always_inline)) int match(const E& elem, ruq_t* q,
                                                ruqelem_t* retelem = nullptr) {
    int match_attempts = 0;
    if (print)
        printf("++ [%i] searching matching queue for src %i tag %i\n", elem.rank, elem.peer,
               elem.tag);
    for (ruq_t::iterator iter = q->begin(); iter != q->end(); ++iter) {
        match_attempts++;
        if (elem.peer == ANY_SOURCE || iter->src == ANY_SOURCE || iter->src == elem.peer) {
            if (elem.tag == ANY_TAG || iter->tag == ANY_TAG || iter->tag == elem.tag) {
                if (retelem) *retelem = *iter;
                q->erase(iter);
                return match_attempts;
            }
        }
    }
    return -1;
}

/* Writes one --qstat file: one line per rank. */
template <class Write>
void write_qstat(const gengetopt_args_info& args, const char* suffix, Write&& write) {
    std::string filename = std::string(args.qstat_arg) + "-" + suffix;
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Can't open " << suffix << " data file (" << filename << ")" << std::endl;
        return;
    }
    write(file);
}

void write_pairs(std::ofstream& file,
                 const std::vector<std::vector<std::pair<int, btime_t>>>& per_rank) {
    for (const auto& pairs : per_rank) {
        for (const auto& [n, t] : pairs) file << n << "," << t << " ";
        file << std::endl;
    }
}

template <goalx::Decoder D>
int simulate(goalx::Parser<D>& parser, const goalx::GoalxFileManager& file,
             gengetopt_args_info& args_info, double load_s) {
    using E = Event<D>;

#ifdef STRICT_ORDER
    btime_t aqtime = 0;
#endif

    const int o = args_info.LogGOPS_o_arg;
    const int O = args_info.LogGOPS_O_arg;
    const int g = args_info.LogGOPS_g_arg;
    const int L = args_info.LogGOPS_L_arg;
    const int G = args_info.LogGOPS_G_arg;
    const std::uint32_t S = args_info.LogGOPS_S_arg;

    auto& schedules = parser.schedules();
    const std::uint32_t p = file.num_ranks();
    const int ncpus = static_cast<int>(file.max_cpu()) + 1;
    const int nnics = static_cast<int>(file.max_nic()) + 1;

    Network net(&args_info); // prints the backend before the size line, as in LogGOPSim.cpp
    printf("size: %i (%i CPUs, %i NICs); L=%i, o=%i g=%i, G=%i, O=%i, P=%i, S=%u\n", p, ncpus,
           nnics, L, o, g, G, O, p, S);

    TimelineVisualization tlviz(&args_info, p);
    Noise osnoise(&args_info, p);

    // MPI matching statistics (--qstat); resize zero-initializes
    std::vector<int> rq_max, uq_max;
    std::vector<std::vector<std::pair<int, btime_t>>> rq_matches, uq_matches, rq_misses, uq_misses;
    std::vector<std::vector<btime_t>> rq_times, uq_times;
    if (args_info.qstat_given) {
        rq_max.resize(p);
        uq_max.resize(p);
        rq_matches.resize(p);
        uq_matches.resize(p);
        rq_misses.resize(p);
        uq_misses.resize(p);
        rq_times.resize(p);
        uq_times.resize(p);
    }

    // the active queue
    std::priority_queue<E, std::vector<E>, LaterFirst> aq;
    // the queues for each host
    std::vector<ruq_t> rq(p), uq(p); // receive queue, unexpected queue
    // next available time for o, g(receive) and g(send), all 0
    std::vector<std::vector<btime_t>> nexto(p, std::vector<btime_t>(ncpus)),
        nextgr(p, std::vector<btime_t>(nnics)), nextgs(p, std::vector<btime_t>(nnics));

    const auto tsim = std::chrono::steady_clock::now();

    std::uint64_t num_events = 0;
    std::vector<E> free_ops;
    for (std::uint32_t host = 0; host < p; ++host) {
        free_ops.clear();
        schedules[host].GetExecutableNodes(free_ops);
        // ensure that the free ops are ordered by type (std::sort like the old simulator: same
        // permutation)
        std::sort(free_ops.begin(), free_ops.end(), ByType{});

        num_events += schedules[host].num_nodes();

        for (E& freeop : free_ops) {
            freeop.time = 0;
#ifdef STRICT_ORDER
            freeop.ts = aqtime++;
#endif
            switch (freeop.type) {
            case OP_LOCOP:
                if (print)
                    printf("init %i (%i,%i) loclop: %lu\n", host, freeop.cpu, freeop.nic,
                           (long unsigned int)freeop.size);
                break;
            case OP_SEND:
                if (print)
                    printf("init %i (%i,%i) send to: %i, tag: %i, size: %lu\n", host, freeop.cpu,
                           freeop.nic, freeop.peer, freeop.tag, (long unsigned int)freeop.size);
                break;
            case OP_RECV:
                if (print)
                    printf("init %i (%i,%i) recvs from: %i, tag: %i, size: %lu\n", host, freeop.cpu,
                           freeop.nic, freeop.peer, freeop.tag, (long unsigned int)freeop.size);
                break;
            default:
                printf("not implemented!\n");
            }
            aq.push(freeop);
        }
    }

    bool new_events = true;
    unsigned lastperc = 0;
    std::vector<std::uint32_t> check_hosts; // the hosts that finished something this round
    while (!aq.empty() || new_events) {
        E elem = aq.top();
        aq.pop();
        check_hosts.clear();

        switch (elem.type) {
        case OP_LOCOP: {
            if (print)
                printf("[%i] found loclop of length %lu - t: %lu (CPU: %i)\n", elem.rank,
                       (unsigned long)elem.size, (unsigned long)elem.time, elem.cpu);
            if (nexto[elem.rank][elem.cpu] <= elem.time) {
                // check if OS Noise occurred
                btime_t noise = osnoise.get_noise(elem.rank, elem.time, elem.time + elem.size);
                nexto[elem.rank][elem.cpu] = elem.time + elem.size + noise;
                // satisfy irequires and requires
                schedules[elem.rank].MarkNodeAsStarted(elem.offset);
                schedules[elem.rank].MarkNodeAsDone(elem.offset);
                check_hosts.push_back(elem.rank);
                tlviz.add_loclop(elem.rank, elem.time, elem.time + elem.size, elem.cpu);
            } else {
                if (print) printf("-- locop local o not available -- reinserting\n");
                elem.time = nexto[elem.rank][elem.cpu];
                aq.push(elem);
            }
        } break;

        case OP_SEND: {
            if (print)
                printf("[%i] found send to %i - t: %lu (CPU: %i)\n", elem.rank, elem.peer,
                       (unsigned long)elem.time, elem.cpu);

            if (std::max(nexto[elem.rank][elem.cpu], nextgs[elem.rank][elem.nic]) <=
                elem.time) { // local o,g available!
                if (print) printf("-- satisfy local irequires\n");
                schedules[elem.rank].MarkNodeAsStarted(elem.offset);
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

                // the message travels to the peer: rank and peer swapped
                elem.type = OP_MSG;
                const std::uint32_t h = elem.rank;
                elem.rank = elem.peer;
                elem.peer = h;
                elem.starttime = elem.time;
                elem.time = elem.time + o +
                            L; // arrival time of first byte only (G will be charged at receiver)
#ifdef STRICT_ORDER
                num_events++;       // MSG is a new event
                elem.ts = aqtime++; // new element in queue
#endif
                if (print)
                    printf("-- [%i] send inserting msg to %i, t: %lu\n", h, elem.rank,
                           (unsigned long)elem.time);
                aq.push(elem);

                if (elem.size <= S) { // eager message
                    if (print)
                        printf("-- [%i] eager -- satisfy local requires at t: %lu\n", h,
                               (long unsigned int)elem.starttime + o);
                    // satisfy local requires (rank and peer swapped!)
                    schedules[h].MarkNodeAsDone(elem.offset);
                } else {
                    if (print)
                        printf("-- [%i] start rendezvous message to: %i (offset: %i)\n", h,
                               elem.rank, elem.offset);
                }
            } else { // local o,g unavailable - retry later
                if (print) printf("-- send local o,g not available -- reinserting\n");
                elem.time = std::max(nexto[elem.rank][elem.cpu], nextgs[elem.rank][elem.nic]);
                aq.push(elem);
            }
        } break;

        case OP_RECV: {
            if (print)
                printf("[%i] found recv from %i - t: %lu (CPU: %i)\n", elem.rank, elem.peer,
                       (unsigned long)elem.time, elem.cpu);

            schedules[elem.rank].MarkNodeAsStarted(elem.offset);
            check_hosts.push_back(elem.rank);
            if (print) printf("-- satisfy local irequires\n");

            ruqelem_t matched_elem;
            if (args_info.qstat_given) {
                uq_max[elem.rank] = std::max((int)uq[elem.rank].size(), uq_max[elem.rank]);
            }

            const int match_attempts = match(elem, &uq[elem.rank], &matched_elem);
            if (match_attempts >= 0) { // found it in local UQ
                if (print) printf("-- found in local UQ\n");
                if (args_info.qstat_given) {
                    uq_matches[elem.rank].emplace_back(match_attempts, elem.time);
                    uq_times[elem.rank].push_back(elem.time - matched_elem.starttime);
                }

                if (elem.size > S) { // rendezvous - free remote request
                    // satisfy remote requires (the offset of the remote send)
                    schedules[matched_elem.src].MarkNodeAsDone(matched_elem.offset);
                    check_hosts.push_back(matched_elem.src);

                    // there is no event < elem.time in the queue, so the remote clocks can be set
                    // to elem.time (see LogGOPSim.cpp before changing this)
                    if (nexto[matched_elem.src][elem.cpu] < elem.time)
                        nexto[matched_elem.src][elem.cpu] = elem.time;
                    if (nextgs[matched_elem.src][elem.nic] < elem.time)
                        nextgs[matched_elem.src][elem.nic] = elem.time;

                    if (print)
                        printf("-- satisfy remote requires at host %i (offset: %i)\n", elem.peer,
                               elem.offset);
                    tlviz.add_transmission(elem.rank, matched_elem.src, matched_elem.starttime + o,
                                           elem.time, elem.size, G);
                }
                tlviz.add_orecv(elem.rank, elem.time, elem.time + o, elem.cpu);

                // satisfy local requires
                schedules[elem.rank].MarkNodeAsDone(elem.offset);
                check_hosts.push_back(elem.rank);
                if (print) printf("-- satisfy local requires\n");
            } else { // not found in local UQ - add to RQ
                if (args_info.qstat_given) {
                    uq_misses[elem.rank].emplace_back((int)uq[elem.rank].size(), elem.time);
                }
                if (print) printf("-- not found in local UQ -- add to RQ\n");
                ruqelem_t nelem;
                nelem.size = elem.size;
                nelem.src = elem.peer;
                nelem.tag = elem.tag;
                nelem.offset = elem.offset;
                rq[elem.rank].push_back(nelem);
            }
        } break;

        case OP_MSG: {
            if (print)
                printf("[%i] found msg from %i, t: %lu (CPU: %i)\n", elem.rank, elem.peer,
                       (unsigned long)elem.time, elem.cpu);
            std::uint64_t earliestfinish;
            if ((earliestfinish = net.query(elem.starttime, elem.time, elem.peer, elem.rank,
                                            elem.size, &elem.handle)) <=
                    elem.time /* msg made it through network */
                && std::max(nexto[elem.rank][elem.cpu], nextgr[elem.rank][elem.nic]) <=
                       elem.time /* local o,g available! */) {
                if (print)
                    printf("-- msg o,g available (nexto: %lu, nextgr: %lu)\n",
                           (long unsigned int)nexto[elem.rank][elem.cpu],
                           (long unsigned int)nextgr[elem.rank][elem.nic]);
                // check if OS Noise occurred
                btime_t noise = osnoise.get_noise(elem.rank, elem.time, elem.time + o);
                // the message is only received after G is charged
                nexto[elem.rank][elem.cpu] =
                    elem.time + o + noise + std::max((elem.size - 1) * O, (elem.size - 1) * G);
                nextgr[elem.rank][elem.nic] = elem.time + g + (elem.size - 1) * G;

                if (args_info.qstat_given) {
                    rq_max[elem.rank] = std::max((int)rq[elem.rank].size(), rq_max[elem.rank]);
                }

                ruqelem_t matched_elem;
                const int match_attempts = match(elem, &rq[elem.rank], &matched_elem);
                if (match_attempts >= 0) { // found it in RQ
                    if (args_info.qstat_given) {
                        rq_matches[elem.rank].emplace_back(match_attempts, elem.time);
                        rq_times[elem.rank].push_back(elem.time - matched_elem.starttime);
                    }

                    if (print) printf("-- found in RQ\n");
                    if (elem.size > S) { // rendezvous - free remote request
                        // satisfy remote requires
                        schedules[elem.peer].MarkNodeAsDone(elem.offset);
                        check_hosts.push_back(elem.peer);
                        // see the rendezvous case of OP_RECV
                        if (nexto[elem.peer][elem.cpu] < elem.time)
                            nexto[elem.peer][elem.cpu] = elem.time;
                        if (nextgs[elem.peer][elem.nic] < elem.time)
                            nextgs[elem.peer][elem.nic] = elem.time;

                        if (print) printf("-- satisfy remote requires on host %i\n", elem.peer);
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
                    // satisfy local requires
                    schedules[elem.rank].MarkNodeAsDone(matched_elem.offset);
                    check_hosts.push_back(elem.rank);
                } else { // not in RQ
                    if (args_info.qstat_given) {
                        rq_misses[elem.rank].emplace_back((int)rq[elem.rank].size(), elem.time);
                    }
                    if (print) printf("-- not found in RQ - add to UQ\n");
                    ruqelem_t nelem;
                    nelem.size = elem.size;
                    nelem.src = elem.peer;
                    nelem.tag = elem.tag;
                    nelem.offset = elem.offset;
                    nelem.starttime = elem.time; // when it was started
                    uq[elem.rank].push_back(nelem);
                }
            } else {
                elem.time =
                    std::max(std::max(nexto[elem.rank][elem.cpu], nextgr[elem.rank][elem.nic]),
                             earliestfinish);
                if (print) printf("-- msg o,g not available -- reinserting\n");
                aq.push(elem);
            }
        } break;

        default:
            printf("not supported\n");
            break;
        }

        // only ask the hosts that actually completed something in this round
        new_events = false;
        std::sort(check_hosts.begin(), check_hosts.end());
        check_hosts.erase(std::unique(check_hosts.begin(), check_hosts.end()), check_hosts.end());

        for (const std::uint32_t host : check_hosts) {
            free_ops.clear();
            schedules[host].GetExecutableNodes(free_ops);
            std::sort(free_ops.begin(), free_ops.end(), ByType{});

            for (E& freeop : free_ops) {
                new_events = true;
#ifdef STRICT_ORDER
                freeop.ts = aqtime++;
#endif
                switch (freeop.type) {
                case OP_LOCOP:
                    freeop.time = nexto[host][freeop.cpu];
                    if (print)
                        printf("%i (%i,%i) loclop: %lu, time: %lu, offset: %i\n", host, freeop.cpu,
                               freeop.nic, (long unsigned int)freeop.size,
                               (long unsigned int)freeop.time, freeop.offset);
                    break;
                case OP_SEND:
                    freeop.time = std::max(nexto[host][freeop.cpu], nextgs[host][freeop.nic]);
                    if (print)
                        printf(
                            "%i (%i,%i) send to: %i, tag: %i, size: %lu, time: %lu, offset: %i\n",
                            host, freeop.cpu, freeop.nic, freeop.peer, freeop.tag,
                            (long unsigned int)freeop.size, (long unsigned int)freeop.time,
                            freeop.offset);
                    break;
                case OP_RECV:
                    freeop.time = nexto[host][freeop.cpu];
                    if (print)
                        printf("%i (%i,%i) recvs from: %i, tag: %i, size: %lu, time: %lu, offset: "
                               "%i\n",
                               host, freeop.cpu, freeop.nic, freeop.peer, freeop.tag,
                               (long unsigned int)freeop.size, (long unsigned int)freeop.time,
                               freeop.offset);
                    break;
                default:
                    printf("not implemented!\n");
                }
                aq.push(freeop);
            }
        }

        if (args_info.progress_given) {
            if (num_events / 100 * lastperc < aqtime) {
                printf("progress: %u %% (%llu) ", lastperc, (unsigned long long)aqtime);
                lastperc++;
                std::size_t maxrq = 0, maxuq = 0;
                for (const auto& q : rq) maxrq = std::max(maxrq, q.size());
                for (const auto& q : uq) maxuq = std::max(maxuq, q.size());
                printf("[sizes: aq: %i max rq: %u max uq: %u]\n", (int)aq.size(), (unsigned)maxrq,
                       (unsigned)maxuq);
            }
        }
    }

    const double sim_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tsim).count();
#ifndef STRICT_ORDER
    unsigned long aqtime = 0;
#endif
    printf("PERFORMANCE: Processes: %i \t Events: %lu \t Time: %.6f s \t Speed: %.2f ev/s\n", p,
           (long unsigned int)aqtime, sim_s, sim_s > 0 ? aqtime / sim_s : 0.0);
    if (args_info.timing_given) {
        printf("TIMING: decoder=%s load_s=%.6f sim_s=%.6f events=%lu\n", D::name, load_s, sim_s,
               (long unsigned int)aqtime);
    }

    // check if all queues are empty
    bool ok = true;
    for (std::uint32_t i = 0; i < p; ++i) {
        if (!uq[i].empty()) {
            printf("unexpected queue on host %i contains %lu elements!\n", i,
                   (unsigned long)uq[i].size());
            for (const ruqelem_t& e : uq[i]) printf(" src: %i, tag: %i\n", e.src, e.tag);
            ok = false;
        }
        if (!rq[i].empty()) {
            printf("receive queue on host %i contains %lu elements!\n", i,
                   (unsigned long)rq[i].size());
            for (const ruqelem_t& e : rq[i]) printf(" src: %i, tag: %i\n", e.src, e.tag);
            ok = false;
        }
    }
    if (!ok) return 0; // as the old simulator: a warning, not an error

    if (p <= 16 && !args_info.batchmode_given) { // print all hosts
        printf("Times: \n");
        for (std::uint32_t i = 0; i < p; ++i) {
            std::cout << "Host " << i << ": " << *std::max_element(nexto[i].begin(), nexto[i].end())
                      << "\n";
        }
    } else { // print only the maximum
        btime_t max = 0;
        std::uint32_t host = 0;
        for (std::uint32_t i = 0; i < p; ++i) {
            const btime_t cur = *std::max_element(nexto[i].begin(), nexto[i].end());
            if (cur > max) {
                host = i;
                max = cur;
            }
        }
        std::cout << "Maximum finishing time at host " << host << ": " << max << " ("
                  << (double)max / 1e9 << " s)\n";
    }

    if (args_info.qstat_given) {
        write_qstat(args_info, "rq-max.data", [&](std::ofstream& f) {
            for (int n : rq_max) f << n << std::endl;
        });
        write_qstat(args_info, "uq-max.data", [&](std::ofstream& f) {
            for (int n : uq_max) f << n << std::endl;
        });
        write_qstat(args_info, "rq-hit.data",
                    [&](std::ofstream& f) { write_pairs(f, rq_matches); });
        write_qstat(args_info, "uq-hit.data",
                    [&](std::ofstream& f) { write_pairs(f, uq_matches); });
        write_qstat(args_info, "rq-miss.data",
                    [&](std::ofstream& f) { write_pairs(f, rq_misses); });
        write_qstat(args_info, "uq-miss.data",
                    [&](std::ofstream& f) { write_pairs(f, uq_misses); });
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    gengetopt_args_info args_info;
    if (cmdline_parser(argc, argv, &args_info) != 0) {
        std::println(stderr, "Couldn't parse command line arguments!");
        return EXIT_FAILURE;
    }
    print = args_info.verbose_given;

    // options of the old parser that this simulator does not have (yet)
    const std::pair<bool, const char*> unsupported[] = {
        {args_info.require_given, "--require"},
        {args_info.cpu_from_given, "--cpu-from"},
        {args_info.nic_from_given, "--nic-from"},
        {args_info.print_schema_given, "--print-schema"},
        {args_info.bench_decode_given, "--bench-decode"},
        {args_info.record_calls_given, "--record-calls"},
        {args_info.emit_results_given, "--emit-results"},
    };
    for (const auto& [given, option] : unsupported) {
        if (given) {
            std::println(stderr, "{} is not supported by this simulator", option);
            return EXIT_FAILURE;
        }
    }
    const std::string decoder_name = args_info.decoder_arg;
    if (decoder_name != "auto" && decoder_name != "generic") {
        std::println(stderr, "Unknown decoder '{}' (this simulator has: generic)", decoder_name);
        return EXIT_FAILURE;
    }

    using Decoder = goalx::GenericDecoder<SimulatorNodeProperties>;
    auto [file, load_s] =
        goalx::timed([&] { return goalx::GoalxFileManager{args_info.filename_arg}; });
    auto decoder = Decoder::create(file.schema());
    if (!decoder) {
        std::println(stderr, "The trace cannot be simulated: {}", decoder.error());
        return EXIT_FAILURE;
    }
    goalx::Parser parser{file, *decoder};
    return simulate(parser, file, args_info, load_s);
}
