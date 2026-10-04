#pragma once

#include <cstdint>
#include <vector>
#include "tinyarmsim/uarch/uop.hpp"
#include "tinyarmsim/uarch/config.hpp"

namespace tinyarmsim::uarch {

struct MacroOpFusionConfig {
    bool enabled{true};
    FusionMode mode{FusionMode::CMP_BRANCH};
};

struct MacroOpFusionStats {
    uint64_t fused_pairs{0};
    uint64_t candidate_pairs{0};
    uint64_t ineligible_pairs{0};

    void reset() noexcept {
        fused_pairs = 0;
        candidate_pairs = 0;
        ineligible_pairs = 0;
    }
};

class MacroOpFusionEngine {
public:
    explicit MacroOpFusionEngine(const MacroOpFusionConfig& cfg = {})
        : config_(cfg) {}

    [[nodiscard]] bool is_enabled() const noexcept {
        return config_.enabled && config_.mode != FusionMode::NONE;
    }

    [[nodiscard]] FusionMode get_mode() const noexcept {
        return config_.mode;
    }

    [[nodiscard]] const MacroOpFusionConfig& get_config() const noexcept {
        return config_;
    }

    [[nodiscard]] const MacroOpFusionStats& get_stats() const noexcept {
        return stats_;
    }

    void reset() noexcept {
        stats_.reset();
    }

    [[nodiscard]] bool can_fuse(const UOp& first, const UOp& second) const noexcept {
        if (!is_enabled()) {
            return false;
        }

        if (config_.mode == FusionMode::CMP_BRANCH) {
            // First instruction must be a flag-setting compare/test ALU op (CMP, TST, CMN, TEQ)
            bool is_cmp_op = (first.opcode == Opcode::CMP || first.opcode == Opcode::TST ||
                              first.opcode == Opcode::CMN || first.opcode == Opcode::TEQ);
            if (!is_cmp_op) {
                return false;
            }

            // Second instruction must be a direct conditional branch (cond != AL and cond != NV)
            bool is_cond_branch = second.is_branch && (second.opcode == Opcode::B) &&
                                  (second.cond != ConditionCode::AL && second.cond != ConditionCode::NV);
            if (!is_cond_branch) {
                return false;
            }

            return true;
        }

        return false;
    }

    [[nodiscard]] UOp fuse_pair(const UOp& cmp_uop, const UOp& branch_uop) noexcept {
        UOp fused;
        fused.uop_id = branch_uop.uop_id;
        fused.seq_num = cmp_uop.seq_num;
        fused.pc = cmp_uop.pc;
        fused.raw_inst = cmp_uop.raw_inst;
        fused.is_thumb32 = cmp_uop.is_thumb32 || branch_uop.is_thumb32;
        fused.is_last_uop_of_macro_inst = branch_uop.is_last_uop_of_macro_inst;

        fused.type = UOpType::BRANCH;
        fused.target_port = ExecutionPort::PORT_0_ALU_BRANCH;
        fused.opcode = branch_uop.opcode;
        fused.cond = branch_uop.cond;

        // Compare operands from cmp_uop
        fused.arch_src1 = cmp_uop.arch_src1;
        fused.arch_src2 = cmp_uop.arch_src2;
        fused.arch_src3 = cmp_uop.arch_src3;
        fused.arch_dest = UOp::INVALID_REG;

        // Immediate values: preserve comparison immediate if valid, otherwise branch immediate
        fused.imm = cmp_uop.is_imm_valid ? cmp_uop.imm : branch_uop.imm;
        fused.offset = branch_uop.offset;
        fused.is_imm_valid = cmp_uop.is_imm_valid || branch_uop.is_imm_valid;

        // Branch target & prediction metadata from branch_uop
        fused.is_branch = true;
        fused.pred_taken = branch_uop.pred_taken;
        fused.pred_target = branch_uop.pred_target;
        fused.actual_taken = branch_uop.actual_taken;
        fused.actual_target = branch_uop.actual_target;
        fused.branch_mispredicted = branch_uop.branch_mispredicted;
        fused.branch_pred = branch_uop.branch_pred;
        fused.rat_checkpoint = branch_uop.rat_checkpoint;

        // Fusion metadata
        fused.is_fused = true;
        fused.fused_cmp_opcode = cmp_uop.opcode;
        fused.sets_flags = false; // Flags consumed directly by branch condition evaluation

        return fused;
    }

    [[nodiscard]] std::vector<UOp> fuse_sequence(const std::vector<UOp>& input_uops) {
        if (!is_enabled() || input_uops.size() < 2) {
            return input_uops;
        }

        std::vector<UOp> result;
        result.reserve(input_uops.size());

        for (size_t i = 0; i < input_uops.size(); ++i) {
            if (i + 1 < input_uops.size() && can_fuse(input_uops[i], input_uops[i + 1])) {
                stats_.candidate_pairs++;
                stats_.fused_pairs++;
                result.push_back(fuse_pair(input_uops[i], input_uops[i + 1]));
                i++; // Skip the second uop of the fused pair
            } else {
                if (input_uops[i].opcode == Opcode::CMP || input_uops[i].opcode == Opcode::TST ||
                    input_uops[i].opcode == Opcode::CMN || input_uops[i].opcode == Opcode::TEQ) {
                    stats_.candidate_pairs++;
                    stats_.ineligible_pairs++;
                }
                result.push_back(input_uops[i]);
            }
        }

        return result;
    }

private:
    MacroOpFusionConfig config_;
    MacroOpFusionStats stats_{};
};

} // namespace tinyarmsim::uarch
