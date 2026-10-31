#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/uarch/config.hpp"
#include "tinyarmsim/uarch/cache.hpp"
#include "tinyarmsim/uarch/coherence.hpp"
#include "tinyarmsim/uarch/stats.hpp"

namespace tinyarmsim::uarch {

struct MemResponse {
    uint32_t data{0};
    uint32_t latency_cycles{1};
    bool is_l1_hit{false};
    bool is_l2_hit{false};
    bool is_dram_access{false};
};

class CoherentMemoryHierarchy {
public:
    CoherentMemoryHierarchy(MemoryBus& physical_bus, const UArchConfig& config)
        : physical_bus_(physical_bus), config_(config),
          l2_cache_(config.l2_shared, "Shared_L2"),
          coherence_engine_(config.num_cores, config.default_core.l1d.line_size) {
        
        size_t num_cores = config.num_cores;
        l1i_caches_.reserve(num_cores);
        l1d_caches_.reserve(num_cores);
        private_l2_caches_.reserve(num_cores);

        for (size_t i = 0; i < num_cores; ++i) {
            const auto& core_cfg = config.get_core_config(i);
            auto l1i = std::make_unique<Cache>(core_cfg.l1i, "Core" + std::to_string(i) + "_L1I");
            auto l1d = std::make_unique<Cache>(core_cfg.l1d, "Core" + std::to_string(i) + "_L1D");

            if (core_cfg.private_l2.has_value() && core_cfg.private_l2->is_active()) {
                auto p_l2 = std::make_unique<Cache>(*core_cfg.private_l2, "Core" + std::to_string(i) + "_Private_L2");
                p_l2->set_next_level(nullptr, config.dram_latency_cycles);
                l1i->set_next_level(p_l2.get(), config.dram_latency_cycles);
                l1d->set_next_level(p_l2.get(), config.dram_latency_cycles);
                private_l2_caches_.push_back(std::move(p_l2));
            } else if (l2_cache_.get_config().is_active()) {
                l1i->set_next_level(&l2_cache_, config.dram_latency_cycles);
                l1d->set_next_level(&l2_cache_, config.dram_latency_cycles);
                private_l2_caches_.push_back(nullptr);
            } else {
                l1i->set_next_level(nullptr, config.dram_latency_cycles);
                l1d->set_next_level(nullptr, config.dram_latency_cycles);
                private_l2_caches_.push_back(nullptr);
            }

            l1i_caches_.push_back(std::move(l1i));
            l1d_caches_.push_back(std::move(l1d));
        }
        l2_cache_.set_next_level(nullptr, config.dram_latency_cycles);
    }

    [[nodiscard]] Cache* get_private_l2(size_t core_id) noexcept {
        return (core_id < private_l2_caches_.size()) ? private_l2_caches_[core_id].get() : nullptr;
    }

    [[nodiscard]] size_t get_num_cores() const noexcept { return config_.num_cores; }
    [[nodiscard]] Cache& get_l1i(size_t core_id) { return *l1i_caches_[core_id]; }
    [[nodiscard]] Cache& get_l1d(size_t core_id) { return *l1d_caches_[core_id]; }
    [[nodiscard]] Cache& get_l2() { return l2_cache_; }
    [[nodiscard]] MESICoherenceEngine& get_coherence() { return coherence_engine_; }
    [[nodiscard]] MemoryBus& get_physical_bus() { return physical_bus_; }

    // Core Instruction Fetch (reads from L1I, misses go to L2 / DRAM)
    MemResponse fetch_instruction(size_t core_id, uint32_t addr, uint64_t current_cycle = 0) {
        MemResponse resp{};
        if (core_id >= config_.num_cores) core_id = 0;

        auto& l1i = *l1i_caches_[core_id];
        if (!l1i.get_config().is_active()) {
            if (l2_cache_.get_config().is_active()) {
                auto l2_res = l2_cache_.access(addr, false, current_cycle);
                if (l2_res.hit) {
                    resp.is_l2_hit = true;
                    resp.latency_cycles = l2_res.latency_cycles;
                    resp.data = physical_bus_.read16(addr);
                    return resp;
                }
            }
            resp.is_dram_access = true;
            resp.latency_cycles = config_.dram_latency_cycles;
            resp.data = physical_bus_.read16(addr);
            return resp;
        }

        auto l1_res = l1i.access(addr, false, current_cycle);
        if (l1_res.hit) {
            resp.is_l1_hit = true;
            resp.latency_cycles = l1_res.latency_cycles;
        } else {
            if (l1_res.latency_cycles >= config_.dram_latency_cycles) {
                resp.is_dram_access = true;
            } else {
                resp.is_l2_hit = true;
            }
            resp.latency_cycles = l1_res.latency_cycles;
        }
        resp.data = physical_bus_.read16(addr);
        return resp;
    }

    // Core Data Read (reads from L1D with MESI coherence tracking)
    MemResponse read_data(size_t core_id, uint32_t addr, uint8_t size = 4, uint64_t current_cycle = 0) {
        MemResponse resp{};
        if (core_id >= config_.num_cores) core_id = 0;

        auto& l1d = *l1d_caches_[core_id];
        
        // If L1D is disabled, bypass directly
        if (!l1d.get_config().is_active()) {
            if (l2_cache_.get_config().is_active()) {
                auto l2_res = l2_cache_.access(addr, false, current_cycle);
                if (l2_res.hit) {
                    resp.is_l2_hit = true;
                    resp.latency_cycles = l2_res.latency_cycles;
                    resp.data = read_bus_data(addr, size);
                    return resp;
                }
            }
            resp.is_dram_access = true;
            resp.latency_cycles = config_.dram_latency_cycles;
            resp.data = read_bus_data(addr, size);
            return resp;
        }

        // Check MESI Coherence State
        CoherenceAction coh_act{};
        if (config_.is_mesi_enabled()) {
            coh_act = coherence_engine_.handle_cpu_read(core_id, addr);
        }

        auto l1_res = l1d.access(addr, false, current_cycle);
        if (l1_res.hit) {
            resp.is_l1_hit = true;
            resp.latency_cycles = l1_res.latency_cycles;
        } else {
            if (l1_res.latency_cycles >= config_.dram_latency_cycles) {
                resp.is_dram_access = true;
            } else {
                resp.is_l2_hit = true;
            }
            resp.latency_cycles = l1_res.latency_cycles + (coh_act.latency_cycles > 1 ? coh_act.latency_cycles : 0);
        }
        resp.data = read_bus_data(addr, size);
        return resp;
    }

    // Core Data Write (writes to L1D with MESI coherence upgrade/broadcast)
    MemResponse write_data(size_t core_id, uint32_t addr, uint32_t val, uint8_t size = 4, uint64_t current_cycle = 0) {
        MemResponse resp{};
        if (core_id >= config_.num_cores) core_id = 0;

        // Perform write to physical storage
        write_bus_data(addr, val, size);

        auto& l1d = *l1d_caches_[core_id];
        if (!l1d.get_config().is_active()) {
            if (l2_cache_.get_config().is_active()) {
                auto l2_res = l2_cache_.access(addr, true, current_cycle);
                if (l2_res.hit) {
                    resp.is_l2_hit = true;
                    resp.latency_cycles = l2_res.latency_cycles;
                    return resp;
                }
            }
            resp.is_dram_access = true;
            resp.latency_cycles = config_.dram_latency_cycles;
            return resp;
        }

        // Update MESI Coherence State & Broadcast Invalidation if needed
        CoherenceAction coh_act{};
        if (config_.is_mesi_enabled()) {
            coh_act = coherence_engine_.handle_cpu_write(core_id, addr);
        }

        auto l1_res = l1d.access(addr, true, current_cycle);

        if (l1_res.hit) {
            resp.is_l1_hit = true;
            resp.latency_cycles = l1_res.latency_cycles + (coh_act.latency_cycles > 1 ? coh_act.latency_cycles : 0);
        } else {
            if (l1_res.latency_cycles >= config_.dram_latency_cycles) {
                resp.is_dram_access = true;
            } else {
                resp.is_l2_hit = true;
            }
            resp.latency_cycles = l1_res.latency_cycles + coh_act.latency_cycles;
        }
        return resp;
    }

    // Aggregate statistics
    [[nodiscard]] UArchStats collect_stats() const {
        UArchStats s{};
        s.cores.resize(config_.num_cores);
        for (size_t i = 0; i < config_.num_cores; ++i) {
            s.cores[i].l1i = l1i_caches_[i]->get_stats();
            s.cores[i].l1d = l1d_caches_[i]->get_stats();
        }
        s.l2_shared = l2_cache_.get_stats();
        s.mesi_snoop_requests = coherence_engine_.get_total_snoop_tx();
        s.mesi_invalidations = coherence_engine_.get_total_invalidations();
        return s;
    }

private:
    [[nodiscard]] uint32_t read_bus_data(uint32_t addr, uint8_t size) const {
        if (size == 1) return physical_bus_.read8(addr);
        if (size == 2) return physical_bus_.read16(addr);
        return physical_bus_.read32(addr);
    }

    void write_bus_data(uint32_t addr, uint32_t val, uint8_t size) {
        if (size == 1) physical_bus_.write8(addr, static_cast<uint8_t>(val & 0xFF));
        else if (size == 2) physical_bus_.write16(addr, static_cast<uint16_t>(val & 0xFFFF));
        else physical_bus_.write32(addr, val);
    }

    MemoryBus& physical_bus_;
    UArchConfig config_;
    Cache l2_cache_;
    MESICoherenceEngine coherence_engine_;
    std::vector<std::unique_ptr<Cache>> l1i_caches_;
    std::vector<std::unique_ptr<Cache>> l1d_caches_;
    std::vector<std::unique_ptr<Cache>> private_l2_caches_;
};

} // namespace tinyarmsim::uarch
