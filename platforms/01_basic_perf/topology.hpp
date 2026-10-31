#pragma once

#include <memory>
#include <string>
#include <fstream>
#include <stdexcept>
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

    uint64_t run(uint64_t max_steps = 1000000) {
        uint64_t steps = 0;
        while (steps < max_steps) {
            try {
                interp_.step();
                steps++;
            } catch (const CpuFaultException&) {
                break;
            }
        }
        return steps;
    }

    [[nodiscard]] MemoryBus& bus() noexcept { return bus_; }
    [[nodiscard]] const MemoryBus& bus() const noexcept { return bus_; }
    [[nodiscard]] const uarch::CoreConfig& config() const noexcept { return config_; }
    [[nodiscard]] const ArchitecturalState& state() const noexcept { return state_; }

private:
    MemoryBus bus_;
    uarch::CoreConfig config_;
    ArchitecturalState state_;
    IsaInterpreter interp_;
};

} // namespace tinyarmsim::platform
