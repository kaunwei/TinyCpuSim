#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <chrono>
#include "tinyarmsim/common.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/uarch/config.hpp"
#include "tinyarmsim/uarch/stats.hpp"
#include "tinyarmsim/uarch/cache.hpp"
#include "tinyarmsim/uarch/coherence.hpp"
#include "tinyarmsim/uarch/memory_hierarchy.hpp"
#include "tinyarmsim/uarch/ooo_core.hpp"
#include "tinyarmsim/uarch/slice_manager.hpp"

namespace tinyarmsim::uarch {

class MultiCoreSystem {
public:
    explicit MultiCoreSystem(const UArchConfig& config, size_t ram_size = 64 * 1024 * 1024)
        : config_(config),
          mem_bus_(ram_size),
          mem_hierarchy_(mem_bus_, config) {
        config_.validate();
        init_cores();
    }

    void set_entry_pc(size_t core_id, uint32_t pc) {
        if (core_id >= cores_.size()) throw std::out_of_range("Core ID out of range");
        CoreConfig core_cfg = (core_id < config_.cores.size()) ? config_.cores[core_id] : config_.default_core;
        cores_[core_id] = std::make_unique<OoOCore>(
            core_id,
            core_cfg,
            mem_bus_,
            &mem_hierarchy_.get_l1i(core_id),
            &mem_hierarchy_.get_l1d(core_id),
            pc
        );
        cores_[core_id]->set_profiler(&profiler_);
    }

    void set_slice_manager(SliceManager* sm) noexcept {
        slice_manager_ = sm;
    }

    [[nodiscard]] SliceManager* get_slice_manager() const noexcept {
        return slice_manager_;
    }

    // Step entire multi-core system by 1 clock cycle (Lockstep Round-Robin)
    void tick() {
        if (all_halted()) return;
        simulated_cycles_++;

        // 1. Tick all cores
        for (auto& core : cores_) {
            if (core && !core->is_halted()) {
                core->tick();
            }
        }

        check_and_trigger_slice();
    }

    // Run simulation until all cores halt or max_cycles is reached
    uint64_t run(uint64_t max_cycles = 10000000) {
        auto start_time = std::chrono::high_resolution_clock::now();

        while (simulated_cycles_ < max_cycles && !all_halted()) {
            tick();
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end_time - start_time;
        wall_time_seconds_ = diff.count();

        return simulated_cycles_;
    }

    [[nodiscard]] bool all_halted() const noexcept {
        for (const auto& core : cores_) {
            if (core && !core->is_halted()) return false;
        }
        return true;
    }

    [[nodiscard]] size_t num_cores() const noexcept {
        return cores_.size();
    }

    [[nodiscard]] MemoryBus& get_bus() noexcept {
        return mem_bus_;
    }

    [[nodiscard]] CoherentMemoryHierarchy& get_memory_hierarchy() noexcept {
        return mem_hierarchy_;
    }

    [[nodiscard]] OoOCore& get_core(size_t core_id) {
        if (core_id >= cores_.size()) throw std::out_of_range("Invalid core ID");
        return *cores_[core_id];
    }

    [[nodiscard]] UArchStats collect_stats() const {
        UArchStats stats;
        stats.total_simulated_cycles = simulated_cycles_;
        stats.target_frequency_mhz = 1000.0;
        stats.wall_time_seconds = wall_time_seconds_;

        for (const auto& core : cores_) {
            if (core) {
                stats.cores.push_back(core->get_stats());
            }
        }

        stats.l2_shared = const_cast<CoherentMemoryHierarchy&>(mem_hierarchy_).get_l2().get_stats();
        stats.mesi_snoop_requests = const_cast<CoherentMemoryHierarchy&>(mem_hierarchy_).get_coherence().get_total_snoop_tx();
        stats.mesi_invalidations = const_cast<CoherentMemoryHierarchy&>(mem_hierarchy_).get_coherence().get_total_invalidations();

        return stats;
    }

    [[nodiscard]] const TopDownProfiler& get_profiler() const noexcept {
        return profiler_;
    }

    [[nodiscard]] TopDownProfiler& get_profiler() noexcept {
        return profiler_;
    }

private:
    void check_and_trigger_slice() {
        if (!slice_manager_ || !slice_manager_->get_config().is_active()) return;
        
        bool trigger = false;
        if (slice_manager_->get_config().interval_ticks > 0) {
            trigger = slice_manager_->should_trigger_tick(simulated_cycles_);
        }
        if (!trigger && slice_manager_->get_config().interval_instructions > 0) {
            uint64_t total_insts = 0;
            for (const auto& core : cores_) {
                if (core) total_insts += core->get_committed_instructions();
            }
            trigger = slice_manager_->should_trigger_instruction(total_insts);
        }

        if (trigger) {
            slice_manager_->capture_slice(collect_stats());
        }
    }

    void init_cores() {
        size_t n = config_.num_cores > 0 ? config_.num_cores : 1;
        uint32_t issue_w = config_.default_core.issue_width > 0 ? config_.default_core.issue_width : 2;
        profiler_.init(n, issue_w);
        cores_.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            CoreConfig core_cfg = (i < config_.cores.size()) ? config_.cores[i] : config_.default_core;
            auto core = std::make_unique<OoOCore>(
                i,
                core_cfg,
                mem_bus_,
                &mem_hierarchy_.get_l1i(i),
                &mem_hierarchy_.get_l1d(i),
                0x10000 // Default entry PC
            );
            core->set_profiler(&profiler_);
            cores_.push_back(std::move(core));
        }
    }

    UArchConfig config_;
    MemoryBus mem_bus_;
    CoherentMemoryHierarchy mem_hierarchy_;
    std::vector<std::unique_ptr<OoOCore>> cores_;
    TopDownProfiler profiler_;
    SliceManager* slice_manager_{nullptr};
    uint64_t simulated_cycles_{0};
    double wall_time_seconds_{0.0};
};

} // namespace tinyarmsim::uarch
