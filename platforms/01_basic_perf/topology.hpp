#pragma once

#include <memory>
#include <string>
#include <fstream>
#include <stdexcept>
#include <limits>
#include "tinyarmsim/platform/time_scheduler.hpp"
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/memory/loader.hpp"
#include "tinyarmsim/cpu/state.hpp"
#include "tinyarmsim/cpu/interpreter.hpp"
#include "tinyarmsim/cpu/faults.hpp"
#include "tinyarmsim/uarch/config.hpp"

namespace tinyarmsim::platform {

class BasicPerfPlatform {
public:
    explicit BasicPerfPlatform(const uarch::CoreConfig& config = uarch::CoreConfig{},
                               size_t mem_size = 64 * 1024 * 1024)
        : bus_(mem_size), config_(config), state_(), interp_(state_, bus_) {}

    void load_workload(const std::string& elf_path) {
        std::ifstream stream(elf_path, std::ios::binary);
        if (!stream) {
            throw std::runtime_error("Failed to open workload ELF: " + elf_path);
        }
        Loader::load_elf(stream, bus_, state_);
    }

    // Absolute scheduler deadline in functional interpreter-step ticks.
    RunResult run_until(uint64_t deadline, uint64_t quantum = 1) {
        return scheduler_.run_interpreter(deadline, quantum, interp_);
    }

    // Compatibility entrypoint: budget is relative to current platform time.
    uint64_t run(uint64_t max_steps = 1000000) {
        if (max_steps > std::numeric_limits<uint64_t>::max() - scheduler_.now())
            throw std::overflow_error("platform deadline overflow");
        return run_until(scheduler_.now() + max_steps).progress;
    }

    [[nodiscard]] uint64_t time() const noexcept { return scheduler_.now(); }

    [[nodiscard]] MemoryBus& bus() noexcept { return bus_; }
    [[nodiscard]] const MemoryBus& bus() const noexcept { return bus_; }
    [[nodiscard]] const uarch::CoreConfig& config() const noexcept { return config_; }
    [[nodiscard]] const ArchitecturalState& state() const noexcept { return state_; }

private:
    MemoryBus bus_;
    uarch::CoreConfig config_;
    ArchitecturalState state_;
    IsaInterpreter interp_;
    TimeScheduler scheduler_;
};

} // namespace tinyarmsim::platform
