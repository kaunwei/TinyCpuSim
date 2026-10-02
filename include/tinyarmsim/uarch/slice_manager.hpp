#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <functional>
#include "tinyarmsim/uarch/stats.hpp"

namespace tinyarmsim::uarch {

enum class SliceFormat : uint8_t {
    Text,
    Gem5,
    JSON
};

struct SliceConfig {
    bool enabled{false};
    uint64_t interval_instructions{0}; // 0 = disabled
    uint64_t interval_ticks{0};        // 0 = disabled (clock cycles / ticks)
    std::string output_file{};         // Empty = write to stdout/trace
    bool reset_after_slice{false};     // If true, reset counters after snapshot
    SliceFormat format{SliceFormat::Text};

    [[nodiscard]] bool is_active() const noexcept {
        return enabled && (interval_instructions > 0 || interval_ticks > 0);
    }
};

struct PerfSlice {
    size_t slice_id{0};
    uint64_t start_cycle{0};
    uint64_t end_cycle{0};
    uint64_t start_instruction{0};
    uint64_t end_instruction{0};
    UArchStats stats_cumulative{};
    UArchStats stats_delta{};

    [[nodiscard]] uint64_t slice_cycles() const noexcept {
        return end_cycle >= start_cycle ? end_cycle - start_cycle : 0;
    }

    [[nodiscard]] uint64_t slice_instructions() const noexcept {
        return end_instruction >= start_instruction ? end_instruction - start_instruction : 0;
    }

    [[nodiscard]] double slice_ipc() const noexcept {
        uint64_t cyc = slice_cycles();
        return cyc > 0 ? static_cast<double>(slice_instructions()) / static_cast<double>(cyc) : 0.0;
    }

    [[nodiscard]] std::string format(SliceFormat fmt, bool all_perf = false) const {
        switch (fmt) {
            case SliceFormat::JSON: {
                std::ostringstream oss;
                oss << "{\n"
                    << "  \"slice_id\": " << slice_id << ",\n"
                    << "  \"start_cycle\": " << start_cycle << ",\n"
                    << "  \"end_cycle\": " << end_cycle << ",\n"
                    << "  \"start_instruction\": " << start_instruction << ",\n"
                    << "  \"end_instruction\": " << end_instruction << ",\n"
                    << "  \"slice_cycles\": " << slice_cycles() << ",\n"
                    << "  \"slice_instructions\": " << slice_instructions() << ",\n"
                    << "  \"slice_ipc\": " << slice_ipc() << ",\n"
                    << "  \"delta\": " << stats_delta.format_json() << ",\n"
                    << "  \"cumulative\": " << stats_cumulative.format_json() << "\n"
                    << "}\n";
                return oss.str();
            }
            case SliceFormat::Gem5: {
                return stats_delta.format_gem5();
            }
            case SliceFormat::Text:
            default: {
                std::ostringstream oss;
                oss << "============================================================\n"
                    << "               TinyCpuSim Performance Slice #" << slice_id << "\n"
                    << "============================================================\n"
                    << "Interval Cycles:           [" << start_cycle << " -> " << end_cycle << "] (" << slice_cycles() << " cycles)\n"
                    << "Interval Instructions:     [" << start_instruction << " -> " << end_instruction << "] (" << slice_instructions() << " insts)\n"
                    << "Interval IPC:              " << std::fixed << std::setprecision(3) << slice_ipc() << "\n"
                    << "------------------------------------------------------------\n"
                    << "[ Delta Statistics for Slice #" << slice_id << " ]\n"
                    << stats_delta.format_text(all_perf)
                    << "============================================================\n";
                return oss.str();
            }
        }
    }
};

class SliceManager {
public:
    explicit SliceManager(SliceConfig config = {})
        : config_(std::move(config)) {}

    void set_config(const SliceConfig& config) {
        config_ = config;
    }

    [[nodiscard]] const SliceConfig& get_config() const noexcept {
        return config_;
    }

    [[nodiscard]] SliceConfig& get_config() noexcept {
        return config_;
    }

    [[nodiscard]] bool should_trigger_instruction(uint64_t current_insts) const noexcept {
        if (!config_.is_active() || config_.interval_instructions == 0) return false;
        return (current_insts - last_slice_inst_) >= config_.interval_instructions;
    }

    [[nodiscard]] bool should_trigger_tick(uint64_t current_ticks) const noexcept {
        if (!config_.is_active() || config_.interval_ticks == 0) return false;
        return (current_ticks - last_slice_tick_) >= config_.interval_ticks;
    }

    // Capture a performance snapshot from current simulator stats
    const PerfSlice& capture_slice(const UArchStats& current_stats) {
        PerfSlice slice;
        slice.slice_id = slices_.size();
        slice.start_cycle = last_slice_tick_;
        slice.end_cycle = current_stats.total_simulated_cycles;
        slice.start_instruction = last_slice_inst_;
        slice.end_instruction = current_stats.total_committed_instructions();

        slice.stats_cumulative = current_stats;
        slice.stats_delta = current_stats - last_stats_snapshot_;

        // Update tracking states
        last_slice_tick_ = current_stats.total_simulated_cycles;
        last_slice_inst_ = current_stats.total_committed_instructions();
        last_stats_snapshot_ = current_stats;

        // Write to output file if specified
        if (!config_.output_file.empty()) {
            std::ofstream ofs(config_.output_file, std::ios::app);
            if (ofs.is_open()) {
                ofs << slice.format(config_.format);
            }
        }

        slices_.push_back(std::move(slice));
        return slices_.back();
    }

    [[nodiscard]] const std::vector<PerfSlice>& get_slices() const noexcept {
        return slices_;
    }

    [[nodiscard]] size_t num_slices() const noexcept {
        return slices_.size();
    }

    void reset() noexcept {
        slices_.clear();
        last_stats_snapshot_ = UArchStats{};
        last_slice_tick_ = 0;
        last_slice_inst_ = 0;
    }

private:
    SliceConfig config_{};
    UArchStats last_stats_snapshot_{};
    uint64_t last_slice_tick_{0};
    uint64_t last_slice_inst_{0};
    std::vector<PerfSlice> slices_{};
};

} // namespace tinyarmsim::uarch
