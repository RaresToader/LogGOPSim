#include "goalx/GoalxFileManager.hpp"
#include "goalx/GoalxGenericDecoder.hpp"
#include "goalx/GoalxParser.hpp"
#include "goalx/SimulatorNodeProperties.hpp"

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

using MySerializedGraph = goalx::SerializedGraph<SimulatorNodeProperties, goalx::GenericDecoder>;

using MyParser = goalx::Parser<SimulatorNodeProperties, goalx::GenericDecoder>;

struct TimestampedNode : MySerializedGraph::IdentifiableNode {
    double time;
};

int main() {
    const char* input_filename = "tests/goalx/schedule.bin";
    goalx::GoalxFileManager file_manager{input_filename};
    std::println("Success creating the file manager");

    MyParser parser{file_manager};
    std::println("Success creating the parser");
    std::println("Parser has {} schedules", parser.schedules().size());

    std::filesystem::path output_path = "tests/goalx/schedule.txt";
    std::ofstream output_file;
    output_file.exceptions(std::ios::failbit | std::ios::badbit);
    output_file.open(output_path);
    parser.schedules()[1].write_as_dot(output_file);

    for (auto& schedule : parser.schedules()) {
        schedule.write_as_dot(std::cout);
    }

    return 0;
}