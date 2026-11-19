#include "goalx/GoalxFileManager.hpp"
#include "goalx/GoalxParser.hpp"
#include "goalx/SimulatorNodeProperties.hpp"
#include "goalx/decoders/GoalxGenericDecoder.hpp"

#include <filesystem>
#include <iostream>
#include <print>

/**
 * Tested from root:
 *
 * ```
 * g++ -Wall -Werror -Wextra -fsanitize=address,undefined -g -std=c++23 -Isrc/LogGOPSim
 * tests/goalx/goalx_file_manager_test.cpp -o build/goalx_file_manager_test &&
 * ./build/goalx_file_manager_test.
 * ```
 */

using MyDecoder = goalx::GenericDecoder<SimulatorNodeProperties>;
using MyParser = goalx::Parser<MyDecoder>;

using MySerializedGraph = goalx::SerializedGraph<MyDecoder>;
struct TimestampedNode : MySerializedGraph::IdentifiableNode {
    double time;
};

int main() {
    const char* input_filename = "tests/goalx/schedule.bin";
    goalx::GoalxFileManager file_manager{input_filename};
    std::println("Success creating the file manager!");

    auto decoder_ret = MyDecoder::create(file_manager.schema());
    if (!decoder_ret.has_value()) {
        std::println("Creating the decoder failed: {}", decoder_ret.error());
        std::exit(EXIT_FAILURE);
    }
    MyDecoder& decoder = decoder_ret.value();
    std::println("Success creating the decoder!");

    MyParser parser{file_manager, decoder};
    std::println("Success creating the parser!");

    std::println("Parser has {} schedules", parser.schedules().size());
    std::filesystem::path output_path = "tests/goalx/schedule.txt";
    std::ofstream output_file;
    output_file.exceptions(std::ios::failbit | std::ios::badbit);
    output_file.open(output_path);
    parser.schedules()[1].write_as_dot(output_file);

    for (auto& schedule : parser.schedules()) {
        schedule.write_as_dot(std::cout);
    }

    std::vector<TimestampedNode> out;
    parser.schedules()[1].GetExecutableNodes(out);
    std::println("Frontier size: {}", out.size());
    for (auto& node : out) {
        node.time = 3.14;
        std::println("{{tag: {}, size: {}, type: {}}}", node.tag, node.size, node.type);
    }

    return 0;
}