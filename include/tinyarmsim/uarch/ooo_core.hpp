#pragma once

#include <cstdint>
#include <vector>
#include <deque>
#include <memory>
#include <iostream>
#include "tinyarmsim/common.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/uarch/config.hpp"
#include "tinyarmsim/uarch/stats.hpp"
#include "tinyarmsim/uarch/uop.hpp"
#include "tinyarmsim/uarch/fetch_unit.hpp"
#include "tinyarmsim/uarch/rat_prf.hpp"
#include "tinyarmsim/uarch/rob_issue_queue.hpp"
#include "tinyarmsim/uarch/lsu.hpp"
#include "tinyarmsim/uarch/cache.hpp"
#include "tinyarmsim/uarch/topdown_profiler.hpp"
#include "tinyarmsim/uarch/slice_manager.hpp"

namespace tinyarmsim::uarch {

class OoOCore {
public:
    OoOCore(size_t core_id,
            const CoreConfig& config,
            MemoryBus& bus,
            Cache* l1i,
            Cache* l1d,
            uint32_t entry_pc = 0)
        : core_id_(core_id),
          config_(config),
          bus_(bus),
          l1i_(l1i),
          l1d_(l1d),
          fetch_unit_(entry_pc, bus, l1i, config, config.branch_predictor),
          prf_(config.num_phys_regs > 0 ? config.num_phys_regs : 128),
          free_list_(config.num_phys_regs > 0 ? config.num_phys_regs : 128, 17),
          rob_(config.rob_size > 0 ? config.rob_size : 64),
          iq_(config.rs_size > 0 ? config.rs_size : 32),
          lsu_(config.lsu, l1d, &bus) {
        prf_.write(13, 0x02000000);
        prf_.write(15, entry_pc);
    }

    void set_profiler(TopDownProfiler* profiler) noexcept {
        profiler_ = profiler;
    }

    void set_slice_manager(SliceManager* sm) noexcept {
        slice_manager_ = sm;
    }

    [[nodiscard]] SliceManager* get_slice_manager() const noexcept {
        return slice_manager_;
    }

    void set_debug(bool d) noexcept { debug_ = d; }

    // Advance core by 1 clock cycle using Reverse Stage Traversal
    void tick() {
        if (halted_) return;
        cycles_++;
        lsu_.tick_cycle(cycles_);
        if (debug_) std::cout << "=== CYCLE " << cycles_ << " ===" << std::endl;

        // -------------------------------------------------------------
        // Stage 8: Commit / Retire
        // -------------------------------------------------------------
        stage_commit();

        // -------------------------------------------------------------
        // Stage 7: Writeback & Wakeup
        // -------------------------------------------------------------
        stage_writeback();

        // -------------------------------------------------------------
        // Stage 6: Execute (Multi-port ALUs, AGUs, Branches)
        // -------------------------------------------------------------
        stage_execute();

        // -------------------------------------------------------------
        // Stage 5: Issue / Select from Issue Queue
        // -------------------------------------------------------------
        stage_issue();

        // -------------------------------------------------------------
        // Stage 4: Dispatch & ROB / LSU Allocation
        // -------------------------------------------------------------
        stage_dispatch();

        // -------------------------------------------------------------
        // Stage 3: Rename & RAT / Checkpointing
        // -------------------------------------------------------------
        stage_rename();

        // -------------------------------------------------------------
        // Stage 1 & 2: Fetch & Pre-decode
        // -------------------------------------------------------------
        fetch_unit_.tick();
        if (fetch_unit_.is_halted() && rob_.is_empty() && rename_queue_.empty()) {
            halted_ = true;
        }
    }

    [[nodiscard]] size_t get_core_id() const noexcept {
        return core_id_;
    }

    [[nodiscard]] MemoryBus& get_bus() noexcept {
        return bus_;
    }

    [[nodiscard]] bool is_halted() const noexcept {
        return halted_;
    }

    [[nodiscard]] uint64_t get_cycles() const noexcept {
        return cycles_;
    }

    [[nodiscard]] uint64_t get_committed_instructions() const noexcept {
        return committed_insts_;
    }

    [[nodiscard]] CoreStats get_stats() const noexcept {
        CoreStats s;
        s.cycles = cycles_;
        s.committed_instructions = committed_insts_;
        s.committed_uops = committed_uops_;
        s.rob_full_stalls = rob_full_stalls_;
        s.rs_full_stalls = rs_full_stalls_;
        s.rename_reg_exhaustion_stalls = rename_stalls_;
        s.branch_mispredict_flushes = branch_flushes_;
        s.head_of_rob_stalls = head_of_rob_stalls_;
        
        s.port_alu_uops = port_alu_uops_;
        s.port_mul_uops = port_mul_uops_;
        s.port_div_uops = port_div_uops_;
        s.port_branch_uops = port_branch_uops_;
        s.port_lsu_uops = port_lsu_uops_;

        s.branch = fetch_unit_.get_branch_predictor().get_stats();
        s.branch.predictions = branch_pred_count_;
        s.branch.mispredictions = branch_flushes_;
        s.branch.correct_predictions = (branch_pred_count_ >= branch_flushes_) ? (branch_pred_count_ - branch_flushes_) : 0;
        s.branch.direct_cond = branch_direct_cond_;
        s.branch.direct_uncond = branch_direct_uncond_;
        s.branch.calls = branch_calls_;
        s.branch.returns = branch_returns_;
        s.branch.indirects = branch_indirects_;

        if (l1i_) s.l1i = l1i_->get_stats();
        if (l1d_) s.l1d = l1d_->get_stats();
        s.lsu = lsu_.get_stats();

        if (profiler_) {
            const auto& r = profiler_->get_report(core_id_);
            s.topdown.total_slots = r.total_slots;
            s.topdown.retiring_slots = r.retiring_slots;
            s.topdown.bad_spec_slots = r.bad_spec_slots;
            s.topdown.frontend_slots = r.frontend_slots;
            s.topdown.backend_slots = r.backend_slots;
            s.topdown.retiring_base_alu = r.retiring_base_alu;
            s.topdown.retiring_mem = r.retiring_mem;
            s.topdown.fe_l1i_miss = r.fe_l1i_miss;
            s.topdown.fe_fetch_bubble = r.fe_fetch_bubble;
            s.topdown.be_core_rs_full = r.be_core_rs_full;
            s.topdown.be_core_rob_full = r.be_core_rob_full;
            s.topdown.be_core_freelist_empty = r.be_core_freelist_empty;
            s.topdown.be_mem_l1d_miss = r.be_mem_l1d_miss;
            s.topdown.be_mem_mshr_full = r.be_mem_mshr_full;
            s.topdown.be_mem_l2_miss = r.be_mem_l2_miss;
            s.topdown.be_mem_store_buf_full = r.be_mem_store_buf_full;
        }

        return s;
    }

private:
    [[nodiscard]] static bool evaluate_condition_flags(ConditionCode cond, bool n, bool z, bool c, bool v) noexcept {
        switch (cond) {
            case ConditionCode::EQ: return z;
            case ConditionCode::NE: return !z;
            case ConditionCode::CS: return c;
            case ConditionCode::CC: return !c;
            case ConditionCode::MI: return n;
            case ConditionCode::PL: return !n;
            case ConditionCode::VS: return v;
            case ConditionCode::VC: return !v;
            case ConditionCode::HI: return c && !z;
            case ConditionCode::LS: return !c || z;
            case ConditionCode::GE: return n == v;
            case ConditionCode::LT: return n != v;
            case ConditionCode::GT: return !z && (n == v);
            case ConditionCode::LE: return z || (n != v);
            case ConditionCode::AL: return true;
            case ConditionCode::NV: return false;
        }
        return true;
    }

    void stage_commit() {
        uint32_t commit_count = 0;
        uint32_t max_commit = config_.commit_width > 0 ? config_.commit_width : 4;

        while (commit_count < max_commit && rob_.can_commit_head()) {
            ROBEntry entry = rob_.commit_head();
            const auto& uop = entry.uop;
            if (debug_) {
                std::cout << " [COMMIT] uop type=" << static_cast<int>(uop.type)
                          << " seq=" << uop.seq_num << " pc=0x" << std::hex << uop.pc << std::dec
                          << " rob=" << entry.rob_idx << " lsu_idx=" << uop.lsu_queue_idx << std::endl;
            }

            if (uop.type == UOpType::SVC) {
                if (uop.imm == 0x50) {
                    if (profiler_) profiler_->trigger_m5op(0x50);
                } else if (uop.imm == 0x51) {
                    if (profiler_) profiler_->trigger_m5op(0x51);
                } else {
                    if (profiler_ && uop.imm == 0x52) profiler_->trigger_m5op(0x52);
                    halted_ = true;
                }
            } else if (uop.type == UOpType::HALT) {
                halted_ = true;
            }

            // Commit architectural destination register
            if (uop.arch_dest != UOp::INVALID_REG && uop.arch_dest < 16) {
                rat_.commit(uop.arch_dest, uop.phys_dest);
                // Free superseded old physical register
                if (uop.old_phys_dest >= 17) {
                    free_list_.free(uop.old_phys_dest);
                }
            }

            // Commit flag register
            if (uop.sets_flags) {
                rat_.commit(UOp::ARCH_REG_FLAGS, uop.phys_flags_dest);
                if (uop.old_phys_flags_dest >= 17) {
                    free_list_.free(uop.old_phys_flags_dest);
                }
            }

            // Drain committed store to L1D / memory bus
            if (uop.type == UOpType::STORE_DATA) {
                lsu_.commit_store(uop.lsu_queue_idx);
            } else if (uop.type == UOpType::LOAD || (uop.type == UOpType::RET && uop.opcode == Opcode::LDR)) {
                lsu_.free_load(uop.lsu_queue_idx);
            }

            committed_uops_++;
            if (uop.is_last_uop_of_macro_inst) {
                committed_insts_++;
            }
            if (profiler_) {
                profiler_->record_slot(core_id_, (uop.type == UOpType::LOAD || uop.type == UOpType::STORE_ADDR || uop.type == UOpType::STORE_DATA) 
                    ? SlotType::RetiringMem : SlotType::RetiringBaseAlu);
            }
            commit_count++;
        }

        if (debug_ && !rob_.is_empty() && !rob_.can_commit_head()) {
            const auto& head = rob_.peek_head();
            std::cout << " [ROB_STALL] Head rob_idx=" << head.rob_idx
                      << " valid=" << head.valid << " ready=" << head.ready
                      << " type=" << static_cast<int>(head.uop.type)
                      << " seq=" << head.uop.seq_num
                      << " pc=0x" << std::hex << head.uop.pc << std::dec
                      << " src1_p=" << head.uop.phys_src1 << "(rdy:" << prf_.is_ready(head.uop.phys_src1) << ")"
                      << " src2_p=" << head.uop.phys_src2 << "(rdy:" << prf_.is_ready(head.uop.phys_src2) << ")"
                      << " flags_p=" << head.uop.phys_flags_src << "(rdy:" << prf_.is_ready(head.uop.phys_flags_src) << ")"
                      << " lsu_idx=" << head.uop.lsu_queue_idx << std::endl;
        }
    }

    void stage_writeback() {
        std::vector<UOp> pending;
        for (const auto& uop : exec_to_wb_buffer_) {
            if (cycles_ >= uop.ready_cycle) {
                if (uop.phys_dest != UOp::INVALID_REG && uop.phys_dest < prf_.size()) {
                    prf_.write(uop.phys_dest, uop.mem_data);
                    iq_.wakeup(uop.phys_dest);
                    for (auto& r_uop : rename_queue_) {
                        if (r_uop.phys_src1 == uop.phys_dest) r_uop.src1_ready = true;
                        if (r_uop.phys_src2 == uop.phys_dest) r_uop.src2_ready = true;
                        if (r_uop.phys_src3 == uop.phys_dest) r_uop.src3_ready = true;
                    }
                }
                if (uop.sets_flags && uop.phys_flags_dest < prf_.size()) {
                    prf_.write(uop.phys_flags_dest, uop.flags_val);
                    iq_.wakeup(uop.phys_flags_dest);
                    for (auto& r_uop : rename_queue_) {
                        if (r_uop.phys_flags_src == uop.phys_flags_dest) r_uop.flags_src_ready = true;
                    }
                }
                rob_.mark_completed(uop.rob_idx);
            } else {
                pending.push_back(uop);
            }
        }
        exec_to_wb_buffer_ = std::move(pending);
    }

    void stage_execute() {
        bool mispredicted_branch = false;
        UOp mispredict_uop;
        uint32_t mispredict_target = 0;

        bool memory_violation = false;
        size_t violating_rob_idx = 0;

        for (auto& uop : issued_uops_) {
            // If an earlier branch in this issue bundle already mispredicted, ignore younger uops
            if (mispredicted_branch) {
                if (uop.seq_num > mispredict_uop.seq_num || (uop.seq_num == mispredict_uop.seq_num && rob_.is_younger(uop.rob_idx, mispredict_uop.rob_idx))) {
                    continue;
                }
            }
            if (memory_violation) {
                uint64_t violating_seq = rob_.get_entry(violating_rob_idx).uop.seq_num;
                if (uop.seq_num > violating_seq || (uop.seq_num == violating_seq && (uop.rob_idx == violating_rob_idx || rob_.is_younger(uop.rob_idx, violating_rob_idx)))) {
                    continue;
                }
            }

            uint32_t val1 = (uop.arch_src1 != UOp::INVALID_REG && uop.phys_src1 < prf_.size()) ? prf_.read(uop.phys_src1) : 0;
            uint32_t val2 = uop.is_imm_valid ? uop.imm : ((uop.arch_src2 != UOp::INVALID_REG && uop.phys_src2 < prf_.size()) ? prf_.read(uop.phys_src2) : 0);
            uint32_t val3 = (uop.arch_src3 != UOp::INVALID_REG && uop.phys_src3 < prf_.size()) ? prf_.read(uop.phys_src3) : 0;

            uint32_t result = 0;
            bool sets_dest = (uop.arch_dest != UOp::INVALID_REG && uop.phys_dest < prf_.size());
            bool n = false, z = false, c = false, v = false;
            uint32_t op_lat = 1;

            switch (uop.opcode) {
                case Opcode::MOV:
                    result = uop.is_imm_valid ? uop.imm : ((uop.arch_src2 != UOp::INVALID_REG) ? val2 : val1);
                    n = ((result >> 31) & 1) != 0;
                    z = (result == 0);
                    break;
                case Opcode::MVN:
                    result = uop.is_imm_valid ? ~uop.imm : ~((uop.arch_src2 != UOp::INVALID_REG) ? val2 : val1);
                    n = ((result >> 31) & 1) != 0;
                    z = (result == 0);
                    break;
                case Opcode::MOVW:
                    result = uop.imm & 0xFFFF;
                    break;
                case Opcode::MOVT:
                    result = (val1 & 0x0000FFFF) | (uop.imm << 16);
                    break;
                case Opcode::ADD:
                case Opcode::ADC:
                case Opcode::CMN:
                    result = val1 + val2 + (uop.opcode == Opcode::ADC && flag_c_ ? 1 : 0);
                    n = ((result >> 31) & 1) != 0;
                    z = (result == 0);
                    c = (static_cast<uint64_t>(val1) + static_cast<uint64_t>(val2)) > 0xFFFFFFFFULL;
                    v = ((~(val1 ^ val2) & (val1 ^ result)) >> 31) & 1;
                    if (uop.opcode == Opcode::CMN) sets_dest = false;
                    break;
                case Opcode::SUB:
                case Opcode::SBC:
                case Opcode::CMP:
                    result = val1 - val2 - (uop.opcode == Opcode::SBC && !flag_c_ ? 1 : 0);
                    n = ((result >> 31) & 1) != 0;
                    z = (result == 0);
                    c = (val1 >= val2);
                    v = (((val1 ^ val2) & (val1 ^ result)) >> 31) & 1;
                    if (uop.opcode == Opcode::CMP) sets_dest = false;
                    break;
                case Opcode::RSB:
                    result = val2 - val1;
                    n = ((result >> 31) & 1) != 0;
                    z = (result == 0);
                    c = (val2 >= val1);
                    v = (((val2 ^ val1) & (val2 ^ result)) >> 31) & 1;
                    break;
                case Opcode::MUL:
                    result = val1 * val2;
                    break;
                case Opcode::MLA:
                    result = val1 * val2 + val3;
                    break;
                case Opcode::AND:
                case Opcode::ORR:
                case Opcode::EOR:
                case Opcode::BIC:
                case Opcode::TST:
                case Opcode::TEQ:
                case Opcode::LSL:
                case Opcode::LSR:
                case Opcode::ASR:
                case Opcode::ROR:
                    if (uop.opcode == Opcode::AND || uop.opcode == Opcode::TST) result = val1 & val2;
                    else if (uop.opcode == Opcode::ORR) result = val1 | val2;
                    else if (uop.opcode == Opcode::EOR || uop.opcode == Opcode::TEQ) result = val1 ^ val2;
                    else if (uop.opcode == Opcode::BIC) result = val1 & ~val2;
                    else if (uop.opcode == Opcode::LSL) { uint32_t a = uop.is_imm_valid ? uop.imm : (val2 & 0x1F); result = (a >= 32) ? 0 : (val1 << a); }
                    else if (uop.opcode == Opcode::LSR) { uint32_t a = uop.is_imm_valid ? uop.imm : (val2 & 0x1F); result = (a >= 32) ? 0 : (val1 >> a); }
                    else if (uop.opcode == Opcode::ASR) { uint32_t a = uop.is_imm_valid ? uop.imm : (val2 & 0x1F); result = (a >= 32) ? (((val1 >> 31) & 1) ? 0xFFFFFFFF : 0) : static_cast<uint32_t>(static_cast<int32_t>(val1) >> a); }
                    else if (uop.opcode == Opcode::ROR) { uint32_t a = (uop.is_imm_valid ? uop.imm : val2) & 0x1F; result = a == 0 ? val1 : ((val1 >> a) | (val1 << (32 - a))); }
                    n = ((result >> 31) & 1) != 0;
                    z = (result == 0);
                    if (uop.opcode == Opcode::TST || uop.opcode == Opcode::TEQ) sets_dest = false;
                    break;
                default:
                    break;
            }

            if (uop.sets_flags) {
                uop.flags_val = (n ? 0x80000000u : 0u) | (z ? 0x40000000u : 0u) | (c ? 0x20000000u : 0u) | (v ? 0x10000000u : 0u);
                flag_n_ = n; flag_z_ = z; flag_c_ = c; flag_v_ = v;
            }

            if (uop.type == UOpType::ALU) {
                if (uop.opcode == Opcode::MUL || uop.opcode == Opcode::MLA) port_mul_uops_++;
                else port_alu_uops_++;
            }

            if (uop.type == UOpType::BRANCH || uop.type == UOpType::CALL || (uop.type == UOpType::RET && uop.opcode != Opcode::LDR)) {
                branch_pred_count_++;
                port_branch_uops_++;
                bool actual_taken = false;
                uint32_t actual_target = uop.actual_target;

                if (uop.opcode == Opcode::CBZ) {
                    actual_taken = (val1 == 0);
                } else if (uop.opcode == Opcode::CBNZ) {
                    actual_taken = (val1 != 0);
                } else if (uop.opcode == Opcode::BX || uop.opcode == Opcode::BLX) {
                    actual_taken = true;
                    actual_target = val1 & ~1u;
                    if (uop.opcode == Opcode::BLX) {
                        result = (uop.pc + (uop.is_thumb32 ? 4 : 2)) | 1;
                        sets_dest = true;
                    }
                } else if (uop.opcode == Opcode::BL) {
                    actual_taken = true;
                    result = (uop.pc + (uop.is_thumb32 ? 4 : 2)) | 1;
                    sets_dest = true;
                } else if (uop.cond == ConditionCode::AL) {
                    actual_taken = true;
                } else {
                    uint32_t f = (uop.phys_flags_src != UOp::INVALID_REG && uop.phys_flags_src < prf_.size()) ? prf_.read(uop.phys_flags_src) : 0;
                    bool fn = (f & 0x80000000u) != 0;
                    bool fz = (f & 0x40000000u) != 0;
                    bool fc = (f & 0x20000000u) != 0;
                    bool fv = (f & 0x10000000u) != 0;
                    actual_taken = evaluate_condition_flags(uop.cond, fn, fz, fc, fv);
                }

                uint32_t next_seq_pc = uop.pc + (uop.is_thumb32 ? 4 : 2);
                uint32_t resolved_target = actual_taken ? (actual_target & ~1u) : next_seq_pc;

                bool mispredict = (actual_taken != uop.pred_taken) || 
                                  (actual_taken && ((actual_target & ~1u) != (uop.pred_target & ~1u)));

                BranchType btype = BranchType::DIRECT_COND;
                if (uop.opcode == Opcode::BL) btype = BranchType::DIRECT_CALL;
                else if (uop.opcode == Opcode::BLX) btype = BranchType::INDIRECT_CALL;
                else if (uop.opcode == Opcode::BX && (uop.arch_src1 == 14)) btype = BranchType::RETURN;
                else if (uop.opcode == Opcode::BX) btype = BranchType::INDIRECT_BRANCH;
                else if (uop.cond == ConditionCode::AL) btype = BranchType::DIRECT_UNCOND;

                if (btype == BranchType::DIRECT_COND) branch_direct_cond_++;
                else if (btype == BranchType::DIRECT_UNCOND) branch_direct_uncond_++;
                else if (btype == BranchType::DIRECT_CALL || btype == BranchType::INDIRECT_CALL) branch_calls_++;
                else if (btype == BranchType::RETURN) branch_returns_++;
                else if (btype == BranchType::INDIRECT_BRANCH) branch_indirects_++;

                // Update predictor in FetchUnit
                fetch_unit_.get_branch_predictor().update(uop.pc, actual_taken, actual_target, btype, uop.branch_pred);

                if (mispredict && !mispredicted_branch) {
                    branch_flushes_++;
                    uop.branch_mispredicted = true;
                    mispredicted_branch = true;
                    mispredict_uop = uop;
                    mispredict_target = resolved_target;

                    if (actual_taken != uop.pred_taken) {
                        fetch_unit_.record_mispredict(true);
                    } else {
                        fetch_unit_.record_mispredict(false);
                    }
                }
            } else if (uop.type == UOpType::STORE_ADDR) {
                if (!lsu_.can_issue_store()) {
                    iq_.replay_insert(uop);
                    continue;
                }
                lsu_.record_store_access();
                port_lsu_uops_++;
                uint32_t offset = uop.is_imm_valid ? static_cast<uint32_t>(uop.offset) : val2;
                uop.mem_addr = val1 + offset;
                size_t violating_rob = 0;
                bool violation = lsu_.execute_store_address(uop.lsu_queue_idx, uop.mem_addr, uop.mem_size_bytes, uop.seq_num, violating_rob);
                if (violation && !memory_violation) {
                    memory_violation = true;
                    violating_rob_idx = violating_rob;
                }
            } else if (uop.type == UOpType::STORE_DATA) {
                port_lsu_uops_++;
                uop.mem_data = val1;
                lsu_.execute_store_data(uop.lsu_queue_idx, uop.mem_data);
            } else if (uop.type == UOpType::LOAD || (uop.type == UOpType::RET && uop.opcode == Opcode::LDR)) {
                if (!lsu_.can_issue_load()) {
                    iq_.replay_insert(uop);
                    continue;
                }
                lsu_.record_load_access();
                port_lsu_uops_++;
                uint32_t offset = uop.is_imm_valid ? static_cast<uint32_t>(uop.offset) : val2;
                uop.mem_addr = val1 + offset;
                auto l_res = lsu_.execute_load(uop.lsu_queue_idx, uop.mem_addr, uop.mem_size_bytes, uop.seq_num, uop.is_signed_mem);
                if (!l_res.completed) {
                    // Replay load next cycle when store data is available
                    iq_.replay_insert(uop);
                    continue;
                }
                uop.mem_data = l_res.data;
                result = l_res.data;
                op_lat = l_res.latency_cycles > 0 ? l_res.latency_cycles : 1;
                if (debug_) {
                    std::cout << " [LOAD_EXEC] type=" << static_cast<int>(uop.type) << " addr=0x" << std::hex << uop.mem_addr
                              << " val1=0x" << val1 << " offset=" << std::dec << uop.offset
                              << " data=0x" << std::hex << result << std::dec
                              << " fwd=" << l_res.forwarded << std::endl;
                }
                if (uop.arch_dest == 15 || uop.type == UOpType::RET) {
                    branch_pred_count_++;
                    branch_returns_++;
                    uint32_t actual_target = result & ~1u;
                    bool actual_taken = true;
                    bool mispredict = (!uop.pred_taken) || ((uop.pred_target & ~1u) != actual_target);

                    fetch_unit_.get_branch_predictor().update(uop.pc, actual_taken, actual_target, BranchType::RETURN, uop.branch_pred);

                    if (mispredict && !mispredicted_branch) {
                        branch_flushes_++;
                        uop.branch_mispredicted = true;
                        mispredicted_branch = true;
                        mispredict_uop = uop;
                        mispredict_target = actual_target;

                        if (!uop.pred_taken) {
                            fetch_unit_.record_mispredict(true);
                        } else {
                            fetch_unit_.record_mispredict(false);
                        }
                    }
                }
            }

            if (uop.type == UOpType::MUL || uop.opcode == Opcode::MUL || uop.opcode == Opcode::MLA) {
                op_lat = 3;
            } else if (uop.type == UOpType::DIV) {
                op_lat = 12;
            }

            if (debug_) {
                std::cout << " [EXEC] uop type=" << static_cast<int>(uop.type)
                          << " seq=" << uop.seq_num << " pc=0x" << std::hex << uop.pc << std::dec
                          << " rob=" << uop.rob_idx << " lsu_idx=" << uop.lsu_queue_idx << std::endl;
            }

            if (sets_dest) {
                uop.mem_data = result;
            }

            uop.ready_cycle = cycles_ + (op_lat > 1 ? (op_lat - 1) : 0);
            uop.executed = true;
            exec_to_wb_buffer_.push_back(uop);
        }
        issued_uops_.clear();

        if (mispredicted_branch && memory_violation) {
            if (mispredict_uop.seq_num < rob_.get_entry(violating_rob_idx).uop.seq_num) {
                recover_from_mispredict(mispredict_uop, mispredict_target);
            } else {
                recover_from_memory_violation(violating_rob_idx);
            }
        } else if (mispredicted_branch) {
            recover_from_mispredict(mispredict_uop, mispredict_target);
        } else if (memory_violation) {
            recover_from_memory_violation(violating_rob_idx);
        }
    }

    void stage_issue() {
        uint32_t issue_width = config_.issue_width > 0 ? config_.issue_width : 4;
        auto candidate_uops = iq_.select_and_issue(issue_width, config_.is_ooo());
        
        uint32_t simple_alu_count = 0;
        uint32_t complex_alu_count = 0;
        uint32_t branch_count = 0;
        uint32_t load_count = 0;
        uint32_t store_count = 0;

        uint32_t max_simple_alu = 2;
        uint32_t max_complex_alu = 1;
        uint32_t max_branch = 1;
        uint32_t max_load = 1;
        uint32_t max_store = 1;

        for (const auto& u : candidate_uops) {
            bool accept = true;
            if (u.type == UOpType::MUL || u.type == UOpType::DIV || u.opcode == Opcode::MUL || u.opcode == Opcode::MLA) {
                if (complex_alu_count >= max_complex_alu) accept = false;
                else complex_alu_count++;
            } else if (u.type == UOpType::ALU) {
                if (simple_alu_count >= max_simple_alu) accept = false;
                else simple_alu_count++;
            } else if (u.type == UOpType::BRANCH || u.type == UOpType::CALL || (u.type == UOpType::RET && u.opcode != Opcode::LDR)) {
                if (branch_count >= max_branch) accept = false;
                else branch_count++;
            } else if (u.type == UOpType::LOAD || (u.type == UOpType::RET && u.opcode == Opcode::LDR)) {
                if (load_count >= max_load) accept = false;
                else load_count++;
            } else if (u.type == UOpType::STORE_ADDR || u.type == UOpType::STORE_DATA) {
                if (store_count >= max_store) accept = false;
                else store_count++;
            }

            if (accept) {
                issued_uops_.push_back(u);
            } else {
                iq_.replay_insert(u);
            }
        }

        if (debug_ && !issued_uops_.empty()) {
            std::cout << " [ISSUE] Issued " << issued_uops_.size() << " uops" << std::endl;
            for (const auto& u : issued_uops_) {
                std::cout << "   -> uop type=" << static_cast<int>(u.type)
                          << " seq=" << u.seq_num << " pc=0x" << std::hex << u.pc << std::dec
                          << " rob=" << u.rob_idx << " lsu_idx=" << u.lsu_queue_idx << std::endl;
            }
        }
    }

    void stage_dispatch() {
        uint32_t dispatch_count = 0;
        uint32_t max_dispatch = 6;
        while (!rename_queue_.empty() && dispatch_count < max_dispatch) {
            const auto& uop = rename_queue_.front();
            if (rob_.is_full()) {
                rob_full_stalls_++;
                if (profiler_) profiler_->record_slot(core_id_, SlotType::BackEndCoreRobFull);
                break;
            }
            if (iq_.is_full()) {
                rs_full_stalls_++;
                if (profiler_) profiler_->record_slot(core_id_, SlotType::BackEndCoreRSFull);
                break;
            }
            bool is_load_uop = (uop.type == UOpType::LOAD || (uop.type == UOpType::RET && uop.opcode == Opcode::LDR));
            if (is_load_uop && !lsu_.can_allocate_load()) break;
            if (uop.type == UOpType::STORE_ADDR && !lsu_.can_allocate_store()) break;

            UOp disp_uop = uop;
            if (disp_uop.arch_src1 != UOp::INVALID_REG && disp_uop.arch_src1 < 16) {
                disp_uop.src1_ready = prf_.is_ready(disp_uop.phys_src1);
            }
            if (disp_uop.arch_src2 != UOp::INVALID_REG && disp_uop.arch_src2 < 16) {
                disp_uop.src2_ready = prf_.is_ready(disp_uop.phys_src2);
            }
            if (disp_uop.arch_src3 != UOp::INVALID_REG && disp_uop.arch_src3 < 16) {
                disp_uop.src3_ready = prf_.is_ready(disp_uop.phys_src3);
            }
            if (disp_uop.cond != ConditionCode::AL && disp_uop.type == UOpType::BRANCH && disp_uop.opcode != Opcode::CBZ && disp_uop.opcode != Opcode::CBNZ) {
                disp_uop.flags_src_ready = prf_.is_ready(disp_uop.phys_flags_src);
            }

            if (is_load_uop) {
                disp_uop.lsu_queue_idx = lsu_.allocate_load(disp_uop);
            } else if (disp_uop.type == UOpType::STORE_ADDR) {
                last_allocated_sq_idx_ = lsu_.allocate_store(disp_uop);
                disp_uop.lsu_queue_idx = last_allocated_sq_idx_;
            } else if (disp_uop.type == UOpType::STORE_DATA) {
                disp_uop.lsu_queue_idx = last_allocated_sq_idx_;
            }

            size_t r_idx = rob_.allocate(disp_uop);
            disp_uop.rob_idx = r_idx;
            if (is_load_uop) {
                lsu_.set_rob_idx_load(disp_uop.lsu_queue_idx, r_idx);
            } else if (disp_uop.type == UOpType::STORE_ADDR) {
                lsu_.set_rob_idx_store(disp_uop.lsu_queue_idx, r_idx);
            }

            if (debug_) {
                std::cout << " [DISPATCH] uop type=" << static_cast<int>(disp_uop.type)
                          << " seq=" << disp_uop.seq_num << " pc=0x" << std::hex << disp_uop.pc << std::dec
                          << " rob=" << disp_uop.rob_idx << " lsu_idx=" << disp_uop.lsu_queue_idx << std::endl;
            }

            iq_.insert(disp_uop);
            rename_queue_.pop_front();
            dispatch_count++;
        }
    }

    void stage_rename() {
        uint32_t rename_count = 0;
        uint32_t rename_width = config_.rename_width > 0 ? config_.rename_width : 4;

        if (debug_) {
            std::cout << " [RENAME] free_count=" << free_list_.free_count()
                      << " rename_q=" << rename_queue_.size()
                      << " rob_count=" << rob_.size()
                      << " fetch_has_uops=" << fetch_unit_.has_uops() << std::endl;
        }

        while (rename_count < rename_width && fetch_unit_.has_uops()) {
            if (rename_queue_.size() >= 8) break;

            const auto& peeked_uop = fetch_unit_.peek_uop();
            size_t needed_regs = 0;
            if (peeked_uop.arch_dest != UOp::INVALID_REG && peeked_uop.arch_dest < 16) needed_regs++;
            if (peeked_uop.sets_flags) needed_regs++;

            if (free_list_.free_count() < needed_regs) {
                rename_stalls_++;
                break;
            }

            UOp uop = fetch_unit_.pop_uop();

            // Source operand renaming & dependency check
            if (uop.arch_src1 != UOp::INVALID_REG && uop.arch_src1 < 16) {
                uop.phys_src1 = rat_.get(uop.arch_src1);
                uop.src1_ready = prf_.is_ready(uop.phys_src1);
            }
            if (uop.arch_src2 != UOp::INVALID_REG && uop.arch_src2 < 16) {
                uop.phys_src2 = rat_.get(uop.arch_src2);
                uop.src2_ready = prf_.is_ready(uop.phys_src2);
            }
            if (uop.arch_src3 != UOp::INVALID_REG && uop.arch_src3 < 16) {
                uop.phys_src3 = rat_.get(uop.arch_src3);
                uop.src3_ready = prf_.is_ready(uop.phys_src3);
            }

            // Flag dependency for conditional branches
            if (uop.cond != ConditionCode::AL && uop.type == UOpType::BRANCH && uop.opcode != Opcode::CBZ && uop.opcode != Opcode::CBNZ) {
                uop.phys_flags_src = rat_.get(UOp::ARCH_REG_FLAGS);
                uop.flags_src_ready = prf_.is_ready(uop.phys_flags_src);
            }

            // Destination operand renaming
            if (uop.arch_dest != UOp::INVALID_REG && uop.arch_dest < 16) {
                uop.old_phys_dest = rat_.get(uop.arch_dest);
                uop.phys_dest = free_list_.allocate();
                rat_.set(uop.arch_dest, uop.phys_dest);
                prf_.set_ready(uop.phys_dest, false); // Pending execution
            }

            // Flag destination renaming for instructions that set flags
            if (uop.sets_flags) {
                uop.old_phys_flags_dest = rat_.get(UOp::ARCH_REG_FLAGS);
                uop.phys_flags_dest = free_list_.allocate();
                rat_.set(UOp::ARCH_REG_FLAGS, uop.phys_flags_dest);
                prf_.set_ready(uop.phys_flags_dest, false);
            }

            // If branch, create RAT checkpoint for speculative recovery
            if (uop.is_branch || uop.type == UOpType::BRANCH || uop.type == UOpType::CALL || uop.type == UOpType::RET) {
                uop.rat_checkpoint = rat_.create_checkpoint();
            }

            rename_queue_.push_back(uop);
            rename_count++;
        }
    }

    void recover_from_mispredict(const UOp& branch_uop, uint32_t redirect_target) {
        if (debug_) {
            std::cout << " [MISPREDICT_RECOVER] Branch seq=" << branch_uop.seq_num
                      << " rob=" << branch_uop.rob_idx
                      << " redirect=0x" << std::hex << redirect_target << std::dec << std::endl;
        }
        rob_.flush_younger_than(branch_uop.rob_idx, [&](const UOp& u) {
            if (u.is_branch) {
                fetch_unit_.record_squashed_branch();
            }
            if (u.arch_dest != UOp::INVALID_REG && u.phys_dest >= 17) {
                free_list_.free(u.phys_dest);
                prf_.set_ready(u.phys_dest, true);
            }
            if (u.sets_flags && u.phys_flags_dest >= 17) {
                free_list_.free(u.phys_flags_dest);
                prf_.set_ready(u.phys_flags_dest, true);
            }
        });
        lsu_.flush_younger_than(branch_uop.seq_num);
        iq_.flush_younger_than(branch_uop.seq_num);
        for (const auto& u : rename_queue_) {
            if (u.seq_num > branch_uop.seq_num) {
                if (u.is_branch) {
                    fetch_unit_.record_squashed_branch();
                }
                if (u.arch_dest != UOp::INVALID_REG && u.phys_dest >= 17) {
                    free_list_.free(u.phys_dest);
                    prf_.set_ready(u.phys_dest, true);
                }
                if (u.sets_flags && u.phys_flags_dest >= 17) {
                    free_list_.free(u.phys_flags_dest);
                    prf_.set_ready(u.phys_flags_dest, true);
                }
            }
        }
        exec_to_wb_buffer_.erase(
            std::remove_if(exec_to_wb_buffer_.begin(), exec_to_wb_buffer_.end(),
                           [&](const UOp& u) { return u.seq_num > branch_uop.seq_num || (u.seq_num == branch_uop.seq_num && rob_.is_younger(u.rob_idx, branch_uop.rob_idx)); }),
            exec_to_wb_buffer_.end());
        rename_queue_.erase(
            std::remove_if(rename_queue_.begin(), rename_queue_.end(),
                           [&](const UOp& u) { return u.seq_num > branch_uop.seq_num; }),
            rename_queue_.end());
        rat_.restore_checkpoint(branch_uop.rat_checkpoint);
        fetch_unit_.get_branch_predictor().squash(branch_uop.branch_pred, branch_uop.actual_taken);
        fetch_unit_.flush(redirect_target, 11);
    }

    void recover_from_memory_violation(size_t violating_rob_idx) {
        if (violating_rob_idx >= rob_.capacity()) return;
        const auto& violating_uop = rob_.get_entry(violating_rob_idx).uop;
        uint32_t redirect_pc = violating_uop.pc;
        uint64_t violating_seq = violating_uop.seq_num;

        size_t rob_flush_point = (violating_rob_idx + rob_.capacity() - 1) % rob_.capacity();
        rob_.flush_younger_than(rob_flush_point, [&](const UOp& u) {
            if (u.arch_dest != UOp::INVALID_REG && u.phys_dest >= 17) {
                free_list_.free(u.phys_dest);
                prf_.set_ready(u.phys_dest, true);
            }
            if (u.sets_flags && u.phys_flags_dest >= 17) {
                free_list_.free(u.phys_flags_dest);
                prf_.set_ready(u.phys_flags_dest, true);
            }
        });
        uint64_t lsu_flush_seq = (violating_seq > 0) ? (violating_seq - 1) : 0;
        lsu_.flush_younger_than(lsu_flush_seq);
        iq_.flush_younger_than(lsu_flush_seq);
        for (const auto& u : rename_queue_) {
            if (u.seq_num >= violating_seq) {
                if (u.arch_dest != UOp::INVALID_REG && u.phys_dest >= 17) {
                    free_list_.free(u.phys_dest);
                    prf_.set_ready(u.phys_dest, true);
                }
                if (u.sets_flags && u.phys_flags_dest >= 17) {
                    free_list_.free(u.phys_flags_dest);
                    prf_.set_ready(u.phys_flags_dest, true);
                }
            }
        }
        exec_to_wb_buffer_.erase(
            std::remove_if(exec_to_wb_buffer_.begin(), exec_to_wb_buffer_.end(),
                           [&](const UOp& u) { return u.seq_num > violating_seq || (u.seq_num == violating_seq && (u.rob_idx == violating_rob_idx || rob_.is_younger(u.rob_idx, violating_rob_idx))); }),
            exec_to_wb_buffer_.end());
        rename_queue_.erase(
            std::remove_if(rename_queue_.begin(), rename_queue_.end(),
                           [&](const UOp& u) { return u.seq_num >= violating_seq; }),
            rename_queue_.end());

        rat_.restore_from_commit();
        if (!rob_.is_empty()) {
            size_t curr = rob_.get_head();
            while (curr != rob_.get_tail()) {
                const auto& rentry = rob_.get_entry(curr);
                if (rentry.valid) {
                    if (rentry.uop.arch_dest != UOp::INVALID_REG && rentry.uop.arch_dest < 16) {
                        rat_.set(rentry.uop.arch_dest, rentry.uop.phys_dest);
                        prf_.set_ready(rentry.uop.phys_dest, rentry.ready);
                    }
                    if (rentry.uop.sets_flags) {
                        rat_.set(UOp::ARCH_REG_FLAGS, rentry.uop.phys_flags_dest);
                        prf_.set_ready(rentry.uop.phys_flags_dest, rentry.ready);
                    }
                }
                curr = (curr + 1) % rob_.capacity();
            }
        }
        fetch_unit_.flush(redirect_pc, 4);
    }

    size_t core_id_{0};
    CoreConfig config_;
    MemoryBus& bus_;
    Cache* l1i_{nullptr};
    Cache* l1d_{nullptr};

    FetchUnit fetch_unit_;
    PhysicalRegisterFile prf_;
    FreeList free_list_;
    RegisterAliasTable rat_;
    ReorderBuffer rob_;
    IssueQueue iq_;
    LoadStoreUnit lsu_;
    size_t last_allocated_sq_idx_{0};

    std::deque<UOp> rename_queue_;
    std::vector<UOp> issued_uops_;
    std::vector<UOp> exec_to_wb_buffer_;

    bool flag_n_{false};
    bool flag_z_{false};
    bool flag_c_{false};
    bool flag_v_{false};

    uint64_t cycles_{0};
    uint64_t committed_insts_{0};
    uint64_t committed_uops_{0};
    uint64_t rob_full_stalls_{0};
    uint64_t rs_full_stalls_{0};
    uint64_t rename_stalls_{0};
    uint64_t branch_flushes_{0};
    uint64_t branch_pred_count_{0};
    uint64_t head_of_rob_stalls_{0};

    uint64_t port_alu_uops_{0};
    uint64_t port_mul_uops_{0};
    uint64_t port_div_uops_{0};
    uint64_t port_branch_uops_{0};
    uint64_t port_lsu_uops_{0};

    uint64_t branch_direct_cond_{0};
    uint64_t branch_direct_uncond_{0};
    uint64_t branch_calls_{0};
    uint64_t branch_returns_{0};
    uint64_t branch_indirects_{0};

    bool halted_{false};
    bool debug_{false};
    TopDownProfiler* profiler_{nullptr};
    SliceManager* slice_manager_{nullptr};
};

} // namespace tinyarmsim::uarch
