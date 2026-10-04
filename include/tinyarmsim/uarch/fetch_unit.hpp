#pragma once

#include <cstdint>
#include <vector>
#include <deque>
#include <memory>
#include "tinyarmsim/common.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/decoder.hpp"
#include "tinyarmsim/uarch/config.hpp"
#include "tinyarmsim/uarch/stats.hpp"
#include "tinyarmsim/uarch/uop.hpp"
#include "tinyarmsim/uarch/branch_predictor.hpp"
#include "tinyarmsim/uarch/uop_decoder.hpp"
#include "tinyarmsim/uarch/cache.hpp"

namespace tinyarmsim::uarch {

class FetchUnit {
public:
    FetchUnit(uint32_t start_pc,
              MemoryBus& bus,
              Cache* l1i_cache,
              const CoreConfig& core_cfg,
              const BranchPredictorConfig& bp_cfg)
        : pc_(start_pc & ~1u),
          bus_(bus),
          l1i_(l1i_cache),
          core_cfg_(core_cfg),
          branch_pred_(bp_cfg),
          fetch_width_(core_cfg.fetch_width > 0 ? core_cfg.fetch_width : 4),
          max_queue_size_(16),
          seq_counter_(0),
          fetch_stall_cycles_(0) {}

    void tick() {
        if (fetch_stall_cycles_ > 0) {
            fetch_stall_cycles_--;
            return;
        }
        if (stalled_ || is_halted_) return;

        // Fetch up to fetch_width instructions per cycle
        bool accessed_l1i = false;
        for (uint32_t i = 0; i < fetch_width_; ++i) {
            if (uop_queue_.size() >= max_queue_size_) {
                // Fetch queue is full, stall front-end
                break;
            }

            // Fetch halfword from memory / L1I
            if (pc_ + 2 > bus_.size()) {
                is_halted_ = true;
                break;
            }

            // Access L1I cache once per fetch cycle
            if (!accessed_l1i && l1i_ && l1i_->get_config().is_active()) {
                accessed_l1i = true;
                uint32_t lat = 1;
                auto cache_res = l1i_->access(pc_, false, lat);
                if (!cache_res.hit && cache_res.latency_cycles > 1) {
                    fetch_stall_cycles_ = cache_res.latency_cycles - 1;
                    break; // Stall immediately on cache miss
                }
            }

            uint16_t w1 = bus_.read16(pc_);
            uint32_t current_inst_pc = pc_;
            DecodedInstruction dec_inst;
            uint32_t inst_size = 2;

            if (Decoder::is_32bit_thumb(w1)) {
                if (pc_ + 4 > bus_.size()) {
                    is_halted_ = true;
                    break;
                }
                uint16_t w2 = bus_.read16(pc_ + 2);
                dec_inst = Decoder::decode32(w1, w2, current_inst_pc);
                inst_size = 4;
            } else {
                dec_inst = Decoder::decode16(w1, current_inst_pc);
                inst_size = 2;
            }

            // Advance sequential PC
            pc_ += inst_size;

            // Expand to micro-ops
            seq_counter_++;
            std::vector<UOp> uops = UOpDecoder::decode(dec_inst, current_inst_pc, seq_counter_);

            // Branch prediction evaluation
            bool redirect = false;
            uint32_t redirect_target = 0;
            bool is_svc = false;

            for (auto& uop : uops) {
                if (uop.type == UOpType::SVC || uop.type == UOpType::HALT) {
                    is_svc = true;
                }
                if (uop.is_branch) {
                    BranchType btype = BranchType::DIRECT_COND;
                    bool is_cond = true;

                    if (uop.opcode == Opcode::BL) {
                        btype = BranchType::DIRECT_CALL;
                        is_cond = false;
                    } else if (uop.opcode == Opcode::BLX) {
                        btype = BranchType::INDIRECT_CALL;
                        is_cond = false;
                    } else if (uop.type == UOpType::RET || (uop.opcode == Opcode::BX && uop.arch_src1 == ARCH_REG_LR)) {
                        btype = BranchType::RETURN;
                        is_cond = false;
                    } else if (uop.opcode == Opcode::BX) {
                        btype = BranchType::INDIRECT_BRANCH;
                        is_cond = false;
                    } else if (uop.cond == ConditionCode::AL && uop.opcode == Opcode::B) {
                        btype = BranchType::DIRECT_UNCOND;
                        is_cond = false;
                    }

                    BranchPrediction pred = branch_pred_.predict(current_inst_pc, btype, is_cond);
                    uop.pred_taken = pred.taken;
                    uop.pred_target = pred.target_pc & ~1u;
                    uop.branch_pred = pred;

                    // If branch is unconditional (B / BL), default taken if not in BTB
                    if (uop.opcode == Opcode::B && uop.cond == ConditionCode::AL && !pred.is_branch) {
                        uop.pred_taken = true;
                        uop.pred_target = uop.actual_target & ~1u;
                    } else if (uop.opcode == Opcode::BL && !pred.is_branch) {
                        uop.pred_taken = true;
                        uop.pred_target = uop.actual_target & ~1u;
                    }

                    if (uop.pred_taken) {
                        redirect = true;
                        redirect_target = uop.pred_target & ~1u;
                    }
                }
                uop_queue_.push_back(uop);
            }

            // If SVC/HALT, stall front-end from fetching beyond it
            if (is_svc) {
                stalled_ = true;
                break;
            }

            // If a branch is predicted taken, redirect Fetch PC and stop fetching for this cycle
            if (redirect) {
                pc_ = redirect_target & ~1u;
                break;
            }
        }
    }

    void record_mispredict(bool is_direction_error) noexcept {
        branch_pred_.record_mispredict(is_direction_error);
    }

    void record_squashed_branch() noexcept {
        branch_pred_.record_squashed_branch();
    }

    // Flush front-end on branch misprediction or exception recovery
    void flush(uint32_t target_pc, uint32_t refill_penalty = 0) noexcept {
        for (const auto& u : uop_queue_) {
            if (u.is_branch) {
                record_squashed_branch();
            }
        }
        uop_queue_.clear();
        pc_ = target_pc & ~1u;
        stalled_ = false;
        is_halted_ = false;
        fetch_stall_cycles_ = refill_penalty;
    }

    [[nodiscard]] bool has_uops() const noexcept {
        return !uop_queue_.empty();
    }

    [[nodiscard]] size_t queue_size() const noexcept {
        return uop_queue_.size();
    }

    UOp pop_uop() {
        if (uop_queue_.empty()) {
            throw std::runtime_error("Attempted to pop from empty fetch UOp queue");
        }
        UOp uop = uop_queue_.front();
        uop_queue_.pop_front();
        return uop;
    }

    [[nodiscard]] const UOp& peek_uop(size_t index = 0) const {
        if (index >= uop_queue_.size()) {
            throw std::runtime_error("Attempted to peek beyond fetch UOp queue bounds");
        }
        return uop_queue_[index];
    }

    [[nodiscard]] uint32_t get_pc() const noexcept {
        return pc_;
    }

    void set_pc(uint32_t pc) noexcept {
        pc_ = pc;
    }

    void set_stalled(bool stalled) noexcept {
        stalled_ = stalled;
    }

    [[nodiscard]] bool is_halted() const noexcept {
        return is_halted_;
    }

    [[nodiscard]] CompositeBranchPredictor& get_branch_predictor() noexcept {
        return branch_pred_;
    }

    [[nodiscard]] const CompositeBranchPredictor& get_branch_predictor() const noexcept {
        return branch_pred_;
    }

    [[nodiscard]] uint64_t next_seq_num() noexcept {
        return ++seq_counter_;
    }

    [[nodiscard]] uint64_t get_seq_counter() const noexcept {
        return seq_counter_;
    }


private:
    uint32_t pc_{0};
    MemoryBus& bus_;
    Cache* l1i_{nullptr};
    CoreConfig core_cfg_;
    CompositeBranchPredictor branch_pred_;
    uint32_t fetch_width_{4};
    size_t max_queue_size_{16};
    uint64_t seq_counter_{0};
    uint32_t fetch_stall_cycles_{0};

    std::deque<UOp> uop_queue_;
    bool stalled_{false};
    bool is_halted_{false};
};

} // namespace tinyarmsim::uarch
