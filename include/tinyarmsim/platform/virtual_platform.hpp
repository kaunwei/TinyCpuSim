#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <stdexcept>

#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/loader.hpp"
#include "tinyarmsim/mmio/nvic.hpp"

namespace tinyarmsim::platform {

struct CpuInstance {
    std::string name;
    MemoryBus& bus;
    ArchitecturalState state;
    mmio::NVICDevice nvic;
    IsaInterpreter interpreter;
    bool halted{false};

    CpuInstance(std::string cpu_name, MemoryBus& cpu_bus)
        : name(std::move(cpu_name)),
          bus(cpu_bus),
          state(),
          nvic(),
          interpreter(state, bus) {
        interpreter.attach_nvic(&nvic);
    }
};

class VirtualPlatform {
public:
    VirtualPlatform() = default;

    MemoryBus& create_bus(const std::string& name, size_t size_bytes) {
        auto bus = std::make_unique<MemoryBus>(size_bytes);
        auto* ptr = bus.get();
        buses_[name] = std::move(bus);
        return *ptr;
    }

    MemoryBus* get_bus(const std::string& name) {
        auto it = buses_.find(name);
        return it != buses_.end() ? it->second.get() : nullptr;
    }

    CpuInstance& create_cpu(const std::string& name, MemoryBus& bus) {
        auto cpu = std::make_unique<CpuInstance>(name, bus);
        auto* ptr = cpu.get();
        cpus_.push_back(std::move(cpu));
        cpu_map_[name] = ptr;
        return *ptr;
    }

    CpuInstance* get_cpu(const std::string& name) {
        auto it = cpu_map_.find(name);
        return it != cpu_map_.end() ? it->second : nullptr;
    }

    const std::vector<std::unique_ptr<CpuInstance>>& get_cpus() const {
        return cpus_;
    }

    void load_elf(const std::string& cpu_name, const std::string& elf_path,
                  Loader::ElfLoadMode mode = Loader::ElfLoadMode::Default) {
        auto* cpu = get_cpu(cpu_name);
        if (!cpu) {
            throw std::runtime_error("CPU not found: " + cpu_name);
        }
        std::ifstream stream(elf_path, std::ios::binary);
        if (!stream) {
            throw std::runtime_error("Failed to open ELF file: " + elf_path);
        }
        Loader::load_elf(stream, cpu->bus, cpu->state, mode);
    }

    void boot_cortex_m(const std::string& cpu_name, uint32_t vtor_base = 0x00000000) {
        auto* cpu = get_cpu(cpu_name);
        if (!cpu) {
            throw std::runtime_error("CPU not found: " + cpu_name);
        }
        Loader::boot_cortex_m(cpu->bus, cpu->state, vtor_base);
    }

    bool step_quantum(uint64_t instructions_per_quantum) {
        bool any_active = false;
        for (auto& cpu_ptr : cpus_) {
            auto& cpu = *cpu_ptr;
            if (cpu.halted) continue;

            uint64_t executed = 0;
            while (executed < instructions_per_quantum && !cpu.halted) {
                try {
                    cpu.interpreter.step();
                    executed++;
                } catch (const CpuFaultException&) {
                    cpu.halted = true;
                    break;
                } catch (...) {
                    cpu.halted = true;
                    throw;
                }
            }
            if (!cpu.halted) {
                any_active = true;
            }
        }

        return any_active;
    }

    uint64_t run(uint64_t max_total_instructions, uint64_t quantum_size = 500) {
        uint64_t instructions_step = 0;
        while (instructions_step < max_total_instructions) {
            bool active = step_quantum(quantum_size);
            instructions_step += quantum_size * (cpus_.empty() ? 1 : cpus_.size());
            if (!active) {
                break;
            }
        }
        return instructions_step;
    }

private:
    std::unordered_map<std::string, std::unique_ptr<MemoryBus>> buses_;
    std::vector<std::unique_ptr<CpuInstance>> cpus_;
    std::unordered_map<std::string, CpuInstance*> cpu_map_;
};

} // namespace tinyarmsim::platform
