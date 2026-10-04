#pragma once

#include <cstdint>
#include <vector>
#include <deque>
#include <optional>
#include <stdexcept>
#include <algorithm>
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/uarch/uop.hpp"
#include "tinyarmsim/uarch/config.hpp"
#include "tinyarmsim/uarch/stats.hpp"
#include "tinyarmsim/uarch/cache.hpp"

namespace tinyarmsim::uarch {

struct LoadQueueEntry {
    size_t lq_idx{0};
    UOp uop{};
    uint32_t addr{0};
    uint8_t size_bytes{4};
    bool addr_valid{false};
    bool data_ready{false};
    uint32_t data{0};
    bool forwarded_from_sq{false};
    bool valid{false};
};

struct StoreQueueEntry {
    size_t sq_idx{0};
    uint64_t seq_num{0};
    size_t rob_idx{0};
    uint32_t addr{0};
    uint32_t data{0};
    uint8_t size_bytes{4};
    bool addr_valid{false};
    bool data_valid{false};
    bool committed{false};
    bool valid{false};
};

class LoadStoreUnit {
public:
    LoadStoreUnit(const LsuConfig& cfg, Cache* l1d_cache, MemoryBus* bus = nullptr)
        : config_(cfg),
          l1d_(l1d_cache),
          bus_(bus),
          lq_(cfg.lq_size > 0 ? cfg.lq_size : 16),
          sq_(cfg.sq_size > 0 ? cfg.sq_size : 16),
          lq_capacity_(cfg.lq_size > 0 ? cfg.lq_size : 16),
          sq_capacity_(cfg.sq_size > 0 ? cfg.sq_size : 16),
          lq_count_(0),
          sq_count_(0) {}

    void tick_cycle(uint64_t current_cycle) noexcept {
        if (current_cycle != last_cycle_) {
            last_cycle_ = current_cycle;
            loads_this_cycle_ = 0;
            stores_this_cycle_ = 0;
        }
    }

    [[nodiscard]] bool can_issue_load() const noexcept {
        uint32_t max_loads = config_.num_load_ports > 0 ? config_.num_load_ports : 1;
        return loads_this_cycle_ < max_loads;
    }

    [[nodiscard]] bool can_issue_store() const noexcept {
        uint32_t max_stores = config_.num_store_ports > 0 ? config_.num_store_ports : 1;
        return stores_this_cycle_ < max_stores;
    }

    void record_load_access() noexcept {
        loads_this_cycle_++;
    }

    void record_store_access() noexcept {
        stores_this_cycle_++;
    }

    [[nodiscard]] bool can_allocate_load() const noexcept {
        return lq_count_ < lq_capacity_;
    }

    [[nodiscard]] bool can_allocate_store() const noexcept {
        return sq_count_ < sq_capacity_;
    }

    size_t allocate_load(const UOp& uop) {
        if (!can_allocate_load()) throw std::runtime_error("LoadQueue full");
        for (size_t i = 0; i < lq_capacity_; ++i) {
            if (!lq_[i].valid) {
                lq_[i].lq_idx = i;
                lq_[i].uop = uop;
                lq_[i].addr_valid = false;
                lq_[i].data_ready = false;
                lq_[i].valid = true;
                lq_count_++;
                return i;
            }
        }
        throw std::runtime_error("LoadQueue allocation failed");
    }

    size_t allocate_store(const UOp& uop) {
        if (!can_allocate_store()) throw std::runtime_error("StoreQueue full");
        for (size_t i = 0; i < sq_capacity_; ++i) {
            if (!sq_[i].valid) {
                sq_[i].sq_idx = i;
                sq_[i].seq_num = uop.seq_num;
                sq_[i].rob_idx = uop.rob_idx;
                sq_[i].addr_valid = false;
                sq_[i].data_valid = false;
                sq_[i].committed = false;
                sq_[i].valid = true;
                sq_count_++;
                return i;
            }
        }
        throw std::runtime_error("StoreQueue allocation failed");
    }

    void set_rob_idx_load(size_t lq_idx, size_t rob_idx) noexcept {
        if (lq_idx < lq_capacity_) {
            lq_[lq_idx].uop.rob_idx = rob_idx;
        }
    }

    void set_rob_idx_store(size_t sq_idx, size_t rob_idx) noexcept {
        if (sq_idx < sq_capacity_) {
            sq_[sq_idx].rob_idx = rob_idx;
        }
    }

    // Execute Store Address uop (STA) -> Port 3
    // Returns true if a speculative load memory hazard violation is detected
    bool execute_store_address(size_t sq_idx, uint32_t addr, uint8_t size_bytes, uint64_t store_seq_num, size_t& out_violating_rob_idx) {
        if (sq_idx >= sq_capacity_ || !sq_[sq_idx].valid) {
            std::string reason = (sq_idx >= sq_capacity_) ? "Out of bounds" : ("sq entry invalid (seq_num in sq=" + std::to_string(sq_[sq_idx].seq_num) + ", uop seq_num=" + std::to_string(store_seq_num) + ")");
            throw std::out_of_range("Invalid SQ index " + std::to_string(sq_idx) + " for STA: " + reason);
        }
        sq_[sq_idx].addr = addr;
        sq_[sq_idx].size_bytes = size_bytes;
        sq_[sq_idx].addr_valid = true;

        // Memory Order Buffer (MOB) Disambiguation:
        // Check if any younger speculative load already executed and read from an overlapping address range
        uint32_t s_end = addr + size_bytes;
        for (const auto& lq_entry : lq_) {
            if (lq_entry.valid && lq_entry.addr_valid && lq_entry.data_ready) {
                if (lq_entry.uop.seq_num > store_seq_num) {
                    uint32_t l_end = lq_entry.addr + lq_entry.size_bytes;
                    bool overlap = (addr < l_end) && (lq_entry.addr < s_end);
                    if (overlap) {
                        if (!lq_entry.forwarded_from_sq) {
                            out_violating_rob_idx = lq_entry.uop.rob_idx;
                            stats_.memory_order_violations++;
                            return true; // Memory hazard violation!
                        }
                    }
                }
            }
        }
        return false;
    }

    // Execute Store Data uop (STD) -> Port 4
    void execute_store_data(size_t sq_idx, uint32_t data) {
        if (sq_idx >= sq_capacity_ || !sq_[sq_idx].valid) {
            throw std::out_of_range("Invalid SQ index for STD");
        }
        sq_[sq_idx].data = data;
        sq_[sq_idx].data_valid = true;
    }

    // Execute Load uop (LDA) -> Port 2 / Port 3
    struct LoadResult {
        bool completed{false};
        uint32_t data{0};
        bool forwarded{false};
        uint32_t latency_cycles{1};
    };

    LoadResult execute_load(size_t lq_idx, uint32_t addr, uint8_t size_bytes, uint64_t load_seq_num, bool is_signed = false) {
        if (lq_idx >= lq_capacity_ || !lq_[lq_idx].valid) {
            throw std::out_of_range("Invalid LQ index for load");
        }
        lq_[lq_idx].addr = addr;
        lq_[lq_idx].size_bytes = size_bytes;
        lq_[lq_idx].addr_valid = true;

        LoadResult res;

        // 1. Store-to-Load Forwarding Check (Search older SQ entries)
        if (config_.is_active()) {
            int best_match = -1;
            uint64_t latest_older_seq = 0;

            for (size_t i = 0; i < sq_capacity_; ++i) {
                const auto& sq = sq_[i];
                if (sq.valid && sq.addr_valid && sq.seq_num < load_seq_num) {
                    if (sq.addr == addr && sq.size_bytes == size_bytes) {
                        if (sq.seq_num >= latest_older_seq) {
                            latest_older_seq = sq.seq_num;
                            best_match = static_cast<int>(i);
                        }
                    }
                }
            }

            if (best_match != -1) {
                if (sq_[static_cast<size_t>(best_match)].data_valid) {
                    res.completed = true;
                    res.forwarded = true;
                    res.data = sq_[static_cast<size_t>(best_match)].data;
                    res.latency_cycles = config_.store_forward_latency > 0 ? config_.store_forward_latency : 1;

                    lq_[lq_idx].data = res.data;
                    lq_[lq_idx].data_ready = true;
                    lq_[lq_idx].forwarded_from_sq = true;
                    stats_.forwarded_loads++;
                    stats_.loads++;
                    return res;
                } else {
                    // Match found in SQ but store data is pending - replay load
                    res.completed = false;
                    return res;
                }
            }
        }

        stats_.loads++;

        // 2. L1 Data Cache Access & Memory Bus Read
        uint32_t bus_data = 0;
        if (bus_) {
            if (size_bytes == 1) {
                uint8_t b_val = bus_->read8(addr);
                bus_data = is_signed ? static_cast<uint32_t>(static_cast<int8_t>(b_val)) : b_val;
            } else if (size_bytes == 2) {
                uint16_t h_val = bus_->read16(addr);
                bus_data = is_signed ? static_cast<uint32_t>(static_cast<int16_t>(h_val)) : h_val;
            } else {
                bus_data = bus_->read32(addr);
            }
        }
        res.data = bus_data;

        if (l1d_ && l1d_->get_config().is_active()) {
            uint32_t lat = 1;
            auto cache_res = l1d_->access(addr, false, lat);
            res.completed = true;
            uint32_t c_lat = cache_res.latency_cycles > 0 ? cache_res.latency_cycles : l1d_->get_config().hit_latency_cycles;
            res.latency_cycles = c_lat > 0 ? c_lat : 2;
        } else {
            res.completed = true;
            res.latency_cycles = 2;
        }

        lq_[lq_idx].data = res.data;
        lq_[lq_idx].data_ready = true;
        return res;
    }

    // Commit Store: Drains from SQ to L1D Cache upon ROB retirement
    void commit_store(size_t sq_idx) {
        if (sq_idx >= sq_capacity_ || !sq_[sq_idx].valid) return;
        const auto& sq = sq_[sq_idx];
        if (bus_) {
            if (sq.size_bytes == 1) {
                bus_->write8(sq.addr, static_cast<uint8_t>(sq.data & 0xFF));
            } else if (sq.size_bytes == 2) {
                bus_->write16(sq.addr, static_cast<uint16_t>(sq.data & 0xFFFF));
            } else {
                bus_->write32(sq.addr, sq.data);
            }
        }
        if (l1d_ && l1d_->get_config().is_active()) {
            uint32_t lat = 1;
            l1d_->access(sq.addr, true, lat);
        }
        stats_.stores++;
        sq_[sq_idx].valid = false;
        if (sq_count_ > 0) sq_count_--;
    }

    void free_load(size_t lq_idx) {
        if (lq_idx >= lq_capacity_ || !lq_[lq_idx].valid) return;
        lq_[lq_idx].valid = false;
        if (lq_count_ > 0) lq_count_--;
    }

    // Flush on pipeline squash / misprediction
    void flush_younger_than(uint64_t seq_num) noexcept {
        for (auto& lq : lq_) {
            if (lq.valid && lq.uop.seq_num > seq_num) {
                lq.valid = false;
                if (lq_count_ > 0) lq_count_--;
            }
        }
        for (auto& sq : sq_) {
            if (sq.valid && sq.seq_num > seq_num) {
                sq.valid = false;
                if (sq_count_ > 0) sq_count_--;
            }
        }
    }

    void reset() noexcept {
        for (auto& l : lq_) l.valid = false;
        for (auto& s : sq_) s.valid = false;
        lq_count_ = 0;
        sq_count_ = 0;
    }

    [[nodiscard]] const LsuStats& get_stats() const noexcept { return stats_; }

private:
    LsuConfig config_;
    Cache* l1d_{nullptr};
    MemoryBus* bus_{nullptr};
    std::vector<LoadQueueEntry> lq_;
    std::vector<StoreQueueEntry> sq_;
    size_t lq_capacity_;
    size_t sq_capacity_;
    size_t lq_count_;
    size_t sq_count_;
    uint64_t last_cycle_{0};
    uint32_t loads_this_cycle_{0};
    uint32_t stores_this_cycle_{0};
    LsuStats stats_{};
};

} // namespace tinyarmsim::uarch
