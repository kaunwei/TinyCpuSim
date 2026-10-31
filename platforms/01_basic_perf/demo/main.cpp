#include <iostream>
#include "platforms/01_basic_perf/topology.hpp"

int main() {
    std::cout << "============================================================\n";
    std::cout << " [01_basic_perf] Functional interpreter execution\n";
    std::cout << "============================================================\n";

    tinyarmsim::platform::BasicPerfPlatform platform;
    
    #ifdef PERF_WORKLOAD_ELF
    const char* elf_path = PERF_WORKLOAD_ELF;
    #else
    const char* elf_path = "fibonacci.elf";
    #endif

    try {
        platform.load_workload(elf_path);
        std::cout << " Loaded Workload: " << elf_path << "\n";
        std::cout << " Executing benchmark simulation...\n";
        const auto result = platform.run_until(200000, 64);
        const char* reason = "deadline";
        switch (result.reason) {
        case tinyarmsim::platform::StopReason::Halt: reason = "halt"; break;
        case tinyarmsim::platform::StopReason::Fault: reason = "fault"; break;
        case tinyarmsim::platform::StopReason::Wait: reason = "wait"; break;
        case tinyarmsim::platform::StopReason::Deadline: break;
        }
        std::cout << " Functional steps: " << result.progress << "\n";
        std::cout << " Stop reason:      " << reason << "\n";
        return result.reason == tinyarmsim::platform::StopReason::Halt ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Performance runner error: " << e.what() << "\n";
        return 1;
    }
}
