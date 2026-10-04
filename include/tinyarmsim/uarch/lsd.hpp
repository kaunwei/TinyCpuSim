#pragma once

#include <cstdint>
#include <vector>
#include <deque>
#include "tinyarmsim/uarch/uop.hpp"

namespace tinyarmsim::uarch {

enum class LoopStreamState : uint8_t {
    INACTIVE,
    PROFILING,
    STREAMING
};

struct LoopStreamDetectorConfig {
    bool enabled{true};
    uint32_t min_iterations_to_lock{3};
    uint32_t max_loop_uops{32};
};

struct LoopStreamDetectorStats {
    uint64_t loops_detected{0};
    uint64_t uops_streamed{0};
    uint64_t iterations_streamed{0};
    uint64_t loop_exits{0};

    void reset() {
        loops_detected = 0;
        uops_streamed = 0;
        iterations_streamed = 0;
        loop_exits = 0;
    }
};

class LoopStreamDetector {
public:
    explicit LoopStreamDetector(const LoopStreamDetectorConfig& cfg = {})
        : config_(cfg),
          state_(LoopStreamState::INACTIVE),
          iteration_count_(0),
          loop_start_pc_(0),
          loop_branch_pc_(0),
          stream_index_(0) {}

    void observe_uop(const UOp& uop) {
        if (!config_.enabled) {
            return;
        }

        if (state_ == LoopStreamState::INACTIVE) {
            candidate_uops_.push_back(uop);

            if (candidate_uops_.size() > config_.max_loop_uops) {
                // Exceeded max loop uop capacity; shift out old uops to keep window within max bounds
                candidate_uops_.erase(candidate_uops_.begin());
            }

            // Detect backward branch: target <= branch PC and taken
            bool is_backward_branch = uop.is_branch && uop.actual_taken && (uop.actual_target <= uop.pc);
            if (is_backward_branch) {
                uint32_t target_pc = uop.actual_target;
                // Find where target_pc appears in candidate buffer
                size_t start_idx = candidate_uops_.size();
                for (size_t i = 0; i < candidate_uops_.size(); ++i) {
                    if (candidate_uops_[i].pc == target_pc) {
                        start_idx = i;
                        break;
                    }
                }

                if (start_idx < candidate_uops_.size()) {
                    std::vector<UOp> body(candidate_uops_.begin() + static_cast<std::ptrdiff_t>(start_idx), candidate_uops_.end());
                    if (!body.empty() && body.size() <= config_.max_loop_uops) {
                        loop_start_pc_ = target_pc;
                        loop_branch_pc_ = uop.pc;
                        loop_buffer_ = std::move(body);
                        iteration_count_ = 1;

                        if (iteration_count_ >= config_.min_iterations_to_lock) {
                            state_ = LoopStreamState::STREAMING;
                            stream_index_ = 0;
                            stats_.loops_detected++;
                        } else {
                            state_ = LoopStreamState::PROFILING;
                        }
                        candidate_uops_.clear();
                    } else {
                        candidate_uops_.clear();
                    }
                } else {
                    // Start address not in candidate buffer; clear to start clean observation
                    candidate_uops_.clear();
                }
            }
        } else if (state_ == LoopStreamState::PROFILING) {
            current_iter_uops_.push_back(uop);

            if (current_iter_uops_.size() > config_.max_loop_uops) {
                // Loop body size exceeds capacity, abort profiling
                notify_loop_exit();
                return;
            }

            bool is_backward_branch = uop.is_branch && uop.actual_taken &&
                                      (uop.pc == loop_branch_pc_) && (uop.actual_target == loop_start_pc_);
            if (is_backward_branch) {
                // Check if the loop body matches size
                if (current_iter_uops_.size() == loop_buffer_.size()) {
                    iteration_count_++;
                    current_iter_uops_.clear();

                    if (iteration_count_ >= config_.min_iterations_to_lock) {
                        state_ = LoopStreamState::STREAMING;
                        stream_index_ = 0;
                        stats_.loops_detected++;
                    }
                } else {
                    // Loop body mutated or diverged
                    notify_loop_exit();
                }
            } else if (uop.is_branch && uop.pc == loop_branch_pc_ && !uop.actual_taken) {
                // Branch fell through (exited loop)
                notify_loop_exit();
            }
        }
    }

    bool is_streaming() const {
        return state_ == LoopStreamState::STREAMING;
    }

    LoopStreamState get_state() const {
        return state_;
    }

    uint32_t get_iteration_count() const {
        return iteration_count_;
    }

    size_t get_captured_uop_count() const {
        return loop_buffer_.size();
    }

    bool get_next_streamed_uop(UOp& out_uop) {
        if (state_ != LoopStreamState::STREAMING || loop_buffer_.empty()) {
            return false;
        }

        out_uop = loop_buffer_[stream_index_];
        stream_index_++;
        stats_.uops_streamed++;

        if (stream_index_ >= loop_buffer_.size()) {
            stream_index_ = 0;
            stats_.iterations_streamed++;
        }
        return true;
    }

    void notify_loop_exit() {
        if (state_ == LoopStreamState::STREAMING) {
            stats_.loop_exits++;
        }
        state_ = LoopStreamState::INACTIVE;
        iteration_count_ = 0;
        loop_start_pc_ = 0;
        loop_branch_pc_ = 0;
        stream_index_ = 0;
        loop_buffer_.clear();
        candidate_uops_.clear();
        current_iter_uops_.clear();
    }

    void reset() {
        state_ = LoopStreamState::INACTIVE;
        iteration_count_ = 0;
        loop_start_pc_ = 0;
        loop_branch_pc_ = 0;
        stream_index_ = 0;
        loop_buffer_.clear();
        candidate_uops_.clear();
        current_iter_uops_.clear();
        stats_.reset();
    }

    const LoopStreamDetectorStats& get_stats() const {
        return stats_;
    }

    const LoopStreamDetectorConfig& get_config() const {
        return config_;
    }

private:
    LoopStreamDetectorConfig config_;
    LoopStreamState state_;
    uint32_t iteration_count_;
    uint32_t loop_start_pc_;
    uint32_t loop_branch_pc_;
    size_t stream_index_;

    std::vector<UOp> loop_buffer_;
    std::vector<UOp> candidate_uops_;
    std::vector<UOp> current_iter_uops_;

    LoopStreamDetectorStats stats_{};
};

} // namespace tinyarmsim::uarch
