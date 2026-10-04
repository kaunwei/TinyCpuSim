#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include "tinyarmsim/uarch/config.hpp"
#include "tinyarmsim/uarch/stats.hpp"

namespace tinyarmsim::uarch {

enum class StrideState : uint8_t {
    INITIAL = 0,
    TRANSIENT = 1,
    STEADY = 2,
    NO_PRED = 3
};

struct StrideEntry {
    bool valid{false};
    uint32_t last_addr{0};
    int32_t stride{0};
    uint8_t confidence{0}; // 0: Initial, 1: Transient, 2: Steady
    uint64_t last_access_timestamp{0};
};

class StridePrefetcher {
public:
    explicit StridePrefetcher(size_t table_entries = 64, size_t degree = 1, size_t distance = 1, size_t line_size = 64)
        : table_entries_(table_entries), degree_(degree), distance_(distance), line_size_(line_size) {}

    [[nodiscard]] StrideState get_state(uint32_t pc) const noexcept {
        auto it = table_.find(pc);
        if (it == table_.end() || !it->second.valid) {
            return StrideState::INITIAL;
        }
        if (it->second.confidence >= 2) {
            return StrideState::STEADY;
        }
        if (it->second.confidence == 1) {
            return StrideState::TRANSIENT;
        }
        return StrideState::INITIAL;
    }

    [[nodiscard]] int32_t get_stride(uint32_t pc) const noexcept {
        auto it = table_.find(pc);
        if (it == table_.end() || !it->second.valid) {
            return 0;
        }
        return it->second.stride;
    }

    std::vector<uint32_t> access(uint32_t pc, uint32_t addr) {
        access_count_++;
        std::vector<uint32_t> prefetches;

        auto it = table_.find(pc);
        if (it == table_.end() || !it->second.valid) {
            // First time seeing this PC -> Initial state
            evict_if_needed();
            StrideEntry entry;
            entry.valid = true;
            entry.last_addr = addr;
            entry.stride = 0;
            entry.confidence = 0;
            entry.last_access_timestamp = access_count_;
            table_[pc] = entry;
            return prefetches;
        }

        auto& entry = it->second;
        entry.last_access_timestamp = access_count_;
        int32_t new_stride = static_cast<int32_t>(addr) - static_cast<int32_t>(entry.last_addr);

        if (entry.confidence == 0) {
            // Transition Initial -> Transient
            entry.stride = new_stride;
            entry.confidence = 1;
            entry.last_addr = addr;
            return prefetches;
        }

        if (new_stride == entry.stride) {
            // Stride matches -> increase confidence (up to Steady=2)
            if (entry.confidence < 2) {
                entry.confidence++;
            }
        } else {
            // Stride mismatch -> reduce confidence or retrain
            if (entry.confidence > 0) {
                entry.confidence--;
            }
            if (entry.confidence == 0) {
                entry.stride = new_stride;
                entry.confidence = 1; // Transition to Transient with new stride
            }
        }

        entry.last_addr = addr;

        // Generate prefetches if in Steady state
        if (entry.confidence >= 2 && entry.stride != 0) {
            int32_t effective_stride = entry.stride;
            for (size_t d = 1; d <= degree_; ++d) {
                int64_t target = static_cast<int64_t>(addr) + (static_cast<int64_t>(distance_) - 1 + static_cast<int64_t>(d)) * effective_stride;
                prefetches.push_back(static_cast<uint32_t>(target));
            }
        }

        return prefetches;
    }

    void reset() noexcept {
        table_.clear();
        access_count_ = 0;
    }

private:
    void evict_if_needed() {
        if (table_.size() < table_entries_) {
            return;
        }
        // LRU eviction
        uint32_t oldest_pc = 0;
        uint64_t oldest_time = UINT64_MAX;
        for (const auto& [pc, entry] : table_) {
            if (entry.last_access_timestamp < oldest_time) {
                oldest_time = entry.last_access_timestamp;
                oldest_pc = pc;
            }
        }
        table_.erase(oldest_pc);
    }

    size_t table_entries_{64};
    size_t degree_{1};
    size_t distance_{1};
    size_t line_size_{64};
    uint64_t access_count_{0};
    std::unordered_map<uint32_t, StrideEntry> table_;
};

class NextLinePrefetcher {
public:
    explicit NextLinePrefetcher(size_t line_size = 64, size_t degree = 1)
        : line_size_(line_size), degree_(degree) {}

    std::vector<uint32_t> access(uint32_t addr) const {
        std::vector<uint32_t> prefetches;
        prefetches.reserve(degree_);
        for (size_t d = 1; d <= degree_; ++d) {
            prefetches.push_back(addr + static_cast<uint32_t>(d * line_size_));
        }
        return prefetches;
    }

private:
    size_t line_size_{64};
    size_t degree_{1};
};

} // namespace tinyarmsim::uarch
