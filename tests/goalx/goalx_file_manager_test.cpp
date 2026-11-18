#include "goalx/GoalxFileManager.hpp"
#include "goalx/GoalxGenericDecoder.hpp"
#include "goalx/SimulatorNodeProperties.hpp"

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

int main() {
    const char* filename = "tests/goalx/schedule.bin";
    goalx::GoalxFileManager manager{filename};
    std::println("Success creating the file manager");
    auto dec = goalx::GenericDecoder<SimulatorNodeProperties>::create(manager.schema());
    if (!dec) {
        std::println(stderr, "{}", dec.error());
        return 1;
    }
    std::println("Success creating the generic decoder");
    return 0;
}