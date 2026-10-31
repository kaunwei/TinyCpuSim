#include <iostream>
#include "platforms/01_basic_perf/topology.hpp"

int main() {
    tinyarmsim::platform::BasicPerfPlatform platform;
    std::cout << "[01_basic_perf] Initialized Perf Platform with memory size: " 
              << platform.bus().size() << " bytes\n";
    std::cout << "[01_basic_perf] Platform ready for microarchitectural validation.\n";
    return 0;
}
