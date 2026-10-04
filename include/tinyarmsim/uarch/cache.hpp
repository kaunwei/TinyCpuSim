#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <stdexcept>
#include "tinyarmsim/uarch/config.hpp"
#include "tinyarmsim/uarch/stats.hpp"
#include "tinyarmsim/uarch/prefetcher.hpp"

namespace tinyarmsim::uarch {

struct CacheLine {
    bool valid{false};
    bool dirty{false};
    uint32_t tag{0};
    uint64_t last_access_timestamp{0};
    uint64_t insertion_order{0};
};

struct CacheSet {
    std::vector<CacheLine> lines;
};

struct CacheAccessResult {
    bool hit{false};
    uint32_t latency_cycles{1};
    bool evicted{false};
    bool evicted_dirty{false};
    uint32_t evicted_addr{0};
    bool mshr_allocated{false};
};

struct MSHREntry {
    bool valid{false};
    uint32_t line_addr{0};
    uint64_t ready_cycle{0};
    bool is_write{false};
};

class Cache {
public:
    Cache(const CacheConfig& config, std::string name = "Cache")
        : config_(config), name_(std::move(name)) {
        config_.validate();
        init_geometry();
    }

    [[nodiscard]] const std::string& get_name() const noexcept { return name_; }
    [[nodiscard]] const CacheConfig& get_config() const noexcept { return config_; }
    [[nodiscard]] const CacheStats& get_stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = CacheStats{}; }

    void set_next_level(Cache* next, uint32_t mem_latency = 80) noexcept {
        next_level_ = next;
        mem_latency_cycles_ = mem_latency;
    }

    [[nodiscard]] Cache* get_next_level() const noexcept { return next_level_; }

    [[nodiscard]] uint32_t get_offset_bits() const noexcept { return offset_bits_; }
    [[nodiscard]] uint32_t get_index_bits() const noexcept { return index_bits_; }
    [[nodiscard]] size_t get_num_sets() const noexcept { return num_sets_; }

    [[nodiscard]] uint32_t extract_offset(uint32_t addr) const noexcept {
        return addr & ((1u << offset_bits_) - 1u);
    }

    [[nodiscard]] uint32_t extract_index(uint32_t addr) const noexcept {
        if (num_sets_ <= 1) return 0;
        return (addr >> offset_bits_) & ((1u << index_bits_) - 1u);
    }

    [[nodiscard]] uint32_t extract_tag(uint32_t addr) const noexcept {
        return addr >> (offset_bits_ + index_bits_);
    }

    [[nodiscard]] uint32_t reconstruct_line_addr(uint32_t tag, uint32_t set_idx) const noexcept {
        return (tag << (offset_bits_ + index_bits_)) | (set_idx << offset_bits_);
    }

    // Fast read probe without updating LRU or tracking stats (used for debugging/snooping)
    [[nodiscard]] bool probe(uint32_t addr) const noexcept {
        if (!config_.is_active()) return true;
        uint32_t set_idx = extract_index(addr);
        uint32_t tag = extract_tag(addr);
        const auto& set = sets_[set_idx];
        for (const auto& line : set.lines) {
            if (line.valid && line.tag == tag) {
                return true;
            }
        }
        return false;
    }

    // Access method: Simulates a cache lookup and replacement
    CacheAccessResult access(uint32_t addr, bool is_write, uint64_t current_cycle = 0, uint32_t pc = 0) {
        CacheAccessResult res{};
        if (!config_.is_active()) {
            res.hit = true;
            res.latency_cycles = 0;
            stats_.record_access(true);
            return res;
        }

        if (config_.is_zero_latency()) {
            res.hit = true;
            res.latency_cycles = 0;
            stats_.record_access(true);
            return res;
        }

        uint32_t set_idx = extract_index(addr);
        uint32_t tag = extract_tag(addr);
        auto& set = sets_[set_idx];

        access_counter_++;

        // 1. Check for Hit in Cache Set
        for (auto& line : set.lines) {
            if (line.valid && line.tag == tag) {
                res.hit = true;
                res.latency_cycles = config_.hit_latency_cycles;
                line.last_access_timestamp = access_counter_;
                if (is_write && config_.write_policy == WritePolicy::WRITE_BACK) {
                    line.dirty = true;
                }
                stats_.record_access(true);

                if (stride_prefetcher_ && config_.is_prefetch_enabled()) {
                    trigger_prefetch(pc, addr, current_cycle);
                }
                return res;
            }
        }

        // 2. Cache Miss: Update stats
        stats_.record_access(false);
        res.hit = false;
        uint32_t miss_penalty = 0;
        if (next_level_ && next_level_->get_config().is_active()) {
            auto next_res = next_level_->access(addr, is_write, current_cycle, pc);
            miss_penalty = next_res.latency_cycles;
        } else {
            miss_penalty = mem_latency_cycles_;
        }
        res.latency_cycles = config_.hit_latency_cycles + miss_penalty;

        // 3. Find invalid line or evict via replacement policy
        size_t victim_idx = find_victim_line(set);
        auto& victim = set.lines[victim_idx];

        if (victim.valid) {
            res.evicted = true;
            res.evicted_dirty = victim.dirty;
            res.evicted_addr = reconstruct_line_addr(victim.tag, set_idx);
            stats_.evictions++;
            if (victim.dirty) {
                stats_.writebacks++;
            }
        }

        // Allocate new line
        victim.valid = true;
        victim.tag = tag;
        victim.last_access_timestamp = access_counter_;
        victim.insertion_order = access_counter_;
        victim.dirty = (is_write && config_.write_policy == WritePolicy::WRITE_BACK);

        // Track MSHR allocation
        res.mshr_allocated = allocate_mshr(addr & ~((1u << offset_bits_) - 1u), is_write, current_cycle);

        if (stride_prefetcher_ && config_.is_prefetch_enabled()) {
            trigger_prefetch(pc, addr, current_cycle);
        }

        return res;
    }

    // Invalidate a line (e.g. from MESI snoop invalidate on remote write)
    bool invalidate_line(uint32_t addr) {
        if (!config_.is_active()) return false;
        uint32_t set_idx = extract_index(addr);
        uint32_t tag = extract_tag(addr);
        auto& set = sets_[set_idx];

        for (auto& line : set.lines) {
            if (line.valid && line.tag == tag) {
                line.valid = false;
                line.dirty = false;
                stats_.invalidations++;
                return true;
            }
        }
        return false;
    }

    // Flush all dirty lines (returns list of dirty addresses written back)
    std::vector<uint32_t> flush() {
        std::vector<uint32_t> dirty_addrs;
        if (!config_.is_active()) return dirty_addrs;

        for (size_t set_idx = 0; set_idx < num_sets_; ++set_idx) {
            for (auto& line : sets_[set_idx].lines) {
                if (line.valid && line.dirty) {
                    dirty_addrs.push_back(reconstruct_line_addr(line.tag, static_cast<uint32_t>(set_idx)));
                    line.dirty = false;
                    stats_.writebacks++;
                }
            }
        }
        return dirty_addrs;
    }

    void reset() {
        for (auto& set : sets_) {
            for (auto& line : set.lines) {
                line = CacheLine{};
            }
        }
        for (auto& entry : mshr_) {
            entry = MSHREntry{};
        }
        if (stride_prefetcher_) {
            stride_prefetcher_->reset();
        }
        access_counter_ = 0;
        reset_stats();
    }

private:
    void init_geometry() {
        if (!config_.is_active()) {
            num_sets_ = 1;
            offset_bits_ = 0;
            index_bits_ = 0;
            return;
        }

        num_sets_ = config_.num_sets();
        offset_bits_ = 0;
        while ((1u << offset_bits_) < config_.line_size) {
            offset_bits_++;
        }

        index_bits_ = 0;
        while ((1u << index_bits_) < num_sets_) {
            index_bits_++;
        }

        sets_.resize(num_sets_);
        for (auto& set : sets_) {
            set.lines.resize(config_.associativity);
        }

        mshr_.resize(config_.mshr_entries);

        if (config_.is_prefetch_enabled()) {
            if (config_.prefetcher == PrefetcherType::STRIDE) {
                stride_prefetcher_ = std::make_unique<StridePrefetcher>(
                    64, 1, config_.prefetch_distance, config_.line_size
                );
            }
        }
    }

    void trigger_prefetch(uint32_t pc, uint32_t addr, uint64_t current_cycle) {
        if (!stride_prefetcher_) return;
        auto prefetches = stride_prefetcher_->access(pc, addr);
        for (uint32_t pf_addr : prefetches) {
            uint32_t line_addr = pf_addr & ~((1u << offset_bits_) - 1u);
            stats_.prefetches_issued++;

            // If already present in cache, hit
            if (probe(line_addr)) {
                stats_.prefetch_hits++;
                continue;
            }

            // Must check physical MSHR capacity
            if (!allocate_mshr(line_addr, false, current_cycle)) {
                // Throttled / filtered by MSHR capacity
                stats_.prefetches_mshr_filtered++;
                continue;
            }

            stats_.prefetch_misses++;

            // Allocate line in cache
            uint32_t pf_set_idx = extract_index(line_addr);
            uint32_t pf_tag = extract_tag(line_addr);
            auto& pf_set = sets_[pf_set_idx];
            size_t victim_idx = find_victim_line(pf_set);
            auto& victim = pf_set.lines[victim_idx];

            if (victim.valid) {
                stats_.evictions++;
                if (victim.dirty) {
                    stats_.writebacks++;
                }
            }

            victim.valid = true;
            victim.tag = pf_tag;
            victim.last_access_timestamp = access_counter_;
            victim.insertion_order = access_counter_;
            victim.dirty = false;
        }
    }

    [[nodiscard]] size_t find_victim_line(const CacheSet& set) const {
        // Priority 1: Pick first invalid line
        for (size_t i = 0; i < set.lines.size(); ++i) {
            if (!set.lines[i].valid) return i;
        }

        // Priority 2: Evict according to policy
        if (config_.replacement == ReplacementPolicy::LRU) {
            size_t min_idx = 0;
            uint64_t min_ts = set.lines[0].last_access_timestamp;
            for (size_t i = 1; i < set.lines.size(); ++i) {
                if (set.lines[i].last_access_timestamp < min_ts) {
                    min_ts = set.lines[i].last_access_timestamp;
                    min_idx = i;
                }
            }
            return min_idx;
        } else if (config_.replacement == ReplacementPolicy::FIFO) {
            size_t min_idx = 0;
            uint64_t min_order = set.lines[0].insertion_order;
            for (size_t i = 1; i < set.lines.size(); ++i) {
                if (set.lines[i].insertion_order < min_order) {
                    min_order = set.lines[i].insertion_order;
                    min_idx = i;
                }
            }
            return min_idx;
        }

        // Default: Index 0
        return 0;
    }

    bool allocate_mshr(uint32_t line_addr, bool is_write, uint64_t current_cycle) {
        // Check if already in MSHR
        for (auto& entry : mshr_) {
            if (entry.valid && entry.line_addr == line_addr) {
                return true; // Merged into existing in-flight request
            }
        }
        // Allocate free slot
        for (auto& entry : mshr_) {
            if (!entry.valid) {
                entry.valid = true;
                entry.line_addr = line_addr;
                entry.is_write = is_write;
                entry.ready_cycle = current_cycle + config_.hit_latency_cycles;
                return true;
            }
        }
        return false; // MSHR full
    }

    CacheConfig config_;
    std::string name_;
    size_t num_sets_{0};
    uint32_t offset_bits_{0};
    uint32_t index_bits_{0};
    std::vector<CacheSet> sets_;
    std::vector<MSHREntry> mshr_;
    std::unique_ptr<StridePrefetcher> stride_prefetcher_{nullptr};
    uint64_t access_counter_{0};
    CacheStats stats_{};
    Cache* next_level_{nullptr};
    uint32_t mem_latency_cycles_{80};
};

} // namespace tinyarmsim::uarch
