#include "goalx/GoalxRequirements.hpp"
#include "goalx/GoalxFileManager.hpp"
#include "goalx/GoalxGenericDecoder.hpp"
#include "goalx/GoalxTimedComputation.hpp"
#include "goalx/SimulatorNodeProperties.hpp"
#include "loggopsim_cmdline.h"
#include "Network.hpp"
#include "TimelineVisualization.hpp"
#include "Noise.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <print>
#include <type_traits>
#include <utility>

#define STRICT_ORDER // this is needed to keep order between Send/Recv and LocalOps in NBC case :-/
#define LIST_MATCH // enables debugging the queues (check if empty)
#define HOSTSYNC // this is experimental to count synchronization times induced by message transmissions

using btime_t = uint64_t;

int main(int argc, char** argv) {

#ifdef STRICT_ORDER
    btime_t aqtime=0; 
#endif

    gengetopt_args_info args_info;
    if (cmdline_parser(argc, argv, &args_info) != 0) {
        std::println(stderr, "Couldn't parse command line arguments!");
        std::exit(EXIT_FAILURE);
    }

#ifndef LIST_MATCH
    if( args_info.qstat_given ){
        std::println("WARNING: --qstat option provided, but LogGOPSim was compiled with LIST_MATCH;\n"
                "         statistics on match queue behavior are NOT valid.\n"); 
    }
#endif

    // TODO: these could probably live in a struct
    const int o=args_info.LogGOPS_o_arg;
    const int O=args_info.LogGOPS_O_arg;
    const int g=args_info.LogGOPS_g_arg;
    const int L=args_info.LogGOPS_L_arg;
    const int G=args_info.LogGOPS_G_arg;
    const bool print=args_info.verbose_given;
    const uint32_t S=args_info.LogGOPS_S_arg;

#ifdef __OPTIMIZE__
  const char* build = "optimized";
#else
  const char* build = "debug";
#endif

    auto [fileManager, time_create_schema] = goalx::timed([&args_info] {
        return goalx::GoalxFileManager{args_info.filename_arg};
    });

    Schema schema = fileManager.schema();
    auto decoder{goalx::GenericDecoder<SimulatorNodeProperties>::create(schema)};

    // TODO: I think this should hold the number of ranks in the schedule?
    const int num_ranks = 0;

    // TODO: API to get max value of a particular field
    const int ncpus = 0;
    const int nnics = 0;

    std::println("size: {} ({} CPUs, {} NICs); L={}, o={} g={}, G={}, O={}, P={}, S={}", 
         num_ranks, ncpus, nnics, L, o, g, G, O, num_ranks, S);

    // TODO: decoder throughput only bench   if (args_info.bench_decode_given) {

    Network net(&args_info);
    TimelineVisualization tlviz(&args_info, num_ranks);
    Noise osnoise(&args_info, num_ranks);

    // TODO: Clean these. They only seem to be used is `qstat_given` is set to true, so only for stats
    std::vector<int> rq_max(0);
    std::vector<int> uq_max(0); 
    std::vector< std::vector< std::pair<int,btime_t> > > rq_matches(0);
    std::vector< std::vector< std::pair<int,btime_t> > > uq_matches(0);
    std::vector< std::vector< std::pair<int,btime_t> > > rq_misses(0);
    std::vector< std::vector< std::pair<int,btime_t> > > uq_misses(0);
    std::vector< std::vector<btime_t> > rq_times(0);
    std::vector< std::vector<btime_t> > uq_times(0); 
    if (args_info.qstat_given) {
        rq_max.resize(num_ranks);
        uq_max.resize(num_ranks); 
        for( int i : rq_max ) {
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

}