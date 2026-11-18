#include "goalx/GoalxFileManager.hpp"
#include "goalx/SimulatorNodeProperties.hpp"

#include <print>

/**
 * Tested with: g++ -Wall -Werror -Wextra -fsanitize=address,undefined -g -std=c++23 -Isrc/LogGOPSim
 * tests/goalx/goalx_file_manager_test.cpp -o build/goalx_file_manager_test &&
 * ./build/goalx_file_manager_test.
 */

int main() {
    const char* filename = "tests/goalx/schedule.bin";
    goalx::GoalxFileManager<char, SimulatorNodeProperties> manager{filename};
    std::println("Success");
    return 0;
}