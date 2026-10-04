#pragma once

#include <cstdint>
#include <vector>
#include "tinyarmsim/instruction.hpp"
#include "tinyarmsim/uarch/uop.hpp"
#include "tinyarmsim/uarch/fusion_unit.hpp"

namespace tinyarmsim::uarch {

class UOpDecoder {
public:
    // Decode a sequence of decoded instructions and apply macro-op fusion if enabled
    [[nodiscard]] static std::vector<UOp> decode_sequence(
        const std::vector<std::pair<DecodedInstruction, uint32_t>>& instrs,
        uint64_t start_seq_num,
        MacroOpFusionEngine* fusion_engine = nullptr) {
        std::vector<UOp> uops;
        uint64_t seq = start_seq_num;
        for (const auto& [instr, pc] : instrs) {
            auto decoded = decode(instr, pc, seq++);
            uops.insert(uops.end(), decoded.begin(), decoded.end());
        }
        if (fusion_engine && fusion_engine->is_enabled()) {
            return fusion_engine->fuse_sequence(uops);
        }
        return uops;
    }

    // Expand a single functional DecodedInstruction into 1 or more uOps
    [[nodiscard]] static std::vector<UOp> decode(const DecodedInstruction& instr, uint32_t pc, uint64_t seq_num) {
        std::vector<UOp> uops;

        switch (instr.op) {
            // -------------------------------------------------------------
            // Store Operations: Split into decoupled uop_STA and uop_STD
            // -------------------------------------------------------------
            case Opcode::STR:
            case Opcode::STRB:
            case Opcode::STRH: {
                // 1. Store Address Generation (uop_STA) -> Target: Port 3 Dual AGU
                UOp sta;
                sta.seq_num = seq_num;
                sta.pc = pc;
                sta.raw_inst = instr.raw_hex;
                sta.is_thumb32 = (instr.instr_size == 4);
                sta.type = UOpType::STORE_ADDR;
                sta.target_port = ExecutionPort::PORT_3_DUAL_AGU;
                sta.opcode = instr.op;
                sta.arch_src1 = instr.rn; // Base register
                sta.arch_src2 = instr.is_imm ? UOp::INVALID_REG : instr.rm; // Offset register or immediate
                sta.arch_dest = UOp::INVALID_REG;
                sta.imm = instr.imm;
                sta.offset = static_cast<int32_t>(instr.imm);
                sta.is_imm_valid = instr.is_imm;
                sta.mem_size_bytes = (instr.op == Opcode::STRB) ? 1 : ((instr.op == Opcode::STRH) ? 2 : 4);

                // 2. Store Data Generation (uop_STD) -> Target: Port 4 Store Data
                UOp std;
                std.seq_num = seq_num;
                std.pc = pc;
                std.raw_inst = instr.raw_hex;
                std.is_thumb32 = (instr.instr_size == 4);
                std.type = UOpType::STORE_DATA;
                std.target_port = ExecutionPort::PORT_4_STORE_DATA;
                std.opcode = instr.op;
                std.arch_src1 = instr.rd; // Value register to store
                std.arch_dest = UOp::INVALID_REG;
                std.mem_size_bytes = sta.mem_size_bytes;

                uops.push_back(sta);
                uops.push_back(std);
                break;
            }

            // -------------------------------------------------------------
            // Load Operations: Single LOAD uop -> Target: Port 2 / Port 3
            // -------------------------------------------------------------
            case Opcode::LDR:
            case Opcode::LDRB:
            case Opcode::LDRH:
            case Opcode::LDRSB:
            case Opcode::LDRSH: {
                UOp lda;
                lda.seq_num = seq_num;
                lda.pc = pc;
                lda.raw_inst = instr.raw_hex;
                lda.is_thumb32 = (instr.instr_size == 4);
                lda.type = (instr.rd == 15) ? UOpType::RET : UOpType::LOAD;
                lda.target_port = ExecutionPort::PORT_2_LOAD_AGU;
                lda.opcode = instr.op;
                lda.arch_dest = instr.rd;
                if (instr.rn == 15) {
                    lda.arch_src1 = UOp::INVALID_REG;
                    lda.imm = ((pc + 4) & ~3u) + (instr.is_imm ? instr.imm : 0);
                    lda.offset = static_cast<int32_t>(lda.imm);
                    lda.is_imm_valid = true;
                } else {
                    lda.arch_src1 = instr.rn;
                    lda.arch_src2 = instr.is_imm ? UOp::INVALID_REG : instr.rm;
                    lda.imm = instr.imm;
                    lda.offset = static_cast<int32_t>(instr.imm);
                    lda.is_imm_valid = instr.is_imm;
                }
                lda.mem_size_bytes = (instr.op == Opcode::LDRB || instr.op == Opcode::LDRSB) ? 1 :
                                     ((instr.op == Opcode::LDRH || instr.op == Opcode::LDRSH) ? 2 : 4);
                lda.is_signed_mem = (instr.op == Opcode::LDRSB || instr.op == Opcode::LDRSH);
                if (instr.rd == 15) {
                    lda.is_branch = true;
                }
                uops.push_back(lda);
                break;
            }

            // -------------------------------------------------------------
            // Multi-Register Load/Store (PUSH, POP, LDM, STM)
            // -------------------------------------------------------------
            case Opcode::PUSH: {
                uint32_t count = 0;
                for (uint8_t r = 0; r < 16; ++r) {
                    if ((instr.register_list & (1U << r)) != 0) count++;
                }
                // First adjust SP = SP - count * 4
                UOp sub_sp;
                sub_sp.seq_num = seq_num;
                sub_sp.pc = pc;
                sub_sp.type = UOpType::ALU;
                sub_sp.target_port = ExecutionPort::PORT_0_ALU_BRANCH;
                sub_sp.opcode = Opcode::SUB;
                sub_sp.arch_dest = 13;
                sub_sp.arch_src1 = 13;
                sub_sp.imm = count * 4;
                sub_sp.is_imm_valid = true;
                uops.push_back(sub_sp);

                int32_t curr_offset = 0;
                for (uint8_t r = 0; r < 16; ++r) {
                    if ((instr.register_list & (1U << r)) != 0) {
                        UOp sta;
                        sta.seq_num = seq_num;
                        sta.pc = pc;
                        sta.type = UOpType::STORE_ADDR;
                        sta.target_port = ExecutionPort::PORT_3_DUAL_AGU;
                        sta.opcode = Opcode::STR;
                        sta.arch_src1 = 13; // New SP
                        sta.imm = static_cast<uint32_t>(curr_offset);
                        sta.offset = curr_offset;
                        sta.is_imm_valid = true;
                        sta.mem_size_bytes = 4;

                        UOp std;
                        std.seq_num = seq_num;
                        std.pc = pc;
                        std.type = UOpType::STORE_DATA;
                        std.target_port = ExecutionPort::PORT_4_STORE_DATA;
                        std.opcode = Opcode::STR;
                        std.arch_src1 = r;
                        std.mem_size_bytes = 4;

                        uops.push_back(sta);
                        uops.push_back(std);
                        curr_offset += 4;
                    }
                }
                break;
            }

            case Opcode::STM: {
                uint8_t base_reg = instr.rn;
                uint32_t count = 0;
                for (uint8_t r = 0; r < 16; ++r) {
                    if ((instr.register_list & (1U << r)) != 0) count++;
                }
                int32_t curr_offset = 0;
                for (uint8_t r = 0; r < 16; ++r) {
                    if ((instr.register_list & (1U << r)) != 0) {
                        UOp sta;
                        sta.seq_num = seq_num;
                        sta.pc = pc;
                        sta.type = UOpType::STORE_ADDR;
                        sta.target_port = ExecutionPort::PORT_3_DUAL_AGU;
                        sta.opcode = Opcode::STR;
                        sta.arch_src1 = base_reg;
                        sta.imm = static_cast<uint32_t>(curr_offset);
                        sta.offset = curr_offset;
                        sta.is_imm_valid = true;
                        sta.mem_size_bytes = 4;

                        UOp std;
                        std.seq_num = seq_num;
                        std.pc = pc;
                        std.type = UOpType::STORE_DATA;
                        std.target_port = ExecutionPort::PORT_4_STORE_DATA;
                        std.opcode = Opcode::STR;
                        std.arch_src1 = r;
                        std.mem_size_bytes = 4;

                        uops.push_back(sta);
                        uops.push_back(std);
                        curr_offset += 4;
                    }
                }
                if (instr.writeback) {
                    UOp add_base;
                    add_base.seq_num = seq_num;
                    add_base.pc = pc;
                    add_base.type = UOpType::ALU;
                    add_base.target_port = ExecutionPort::PORT_0_ALU_BRANCH;
                    add_base.opcode = Opcode::ADD;
                    add_base.arch_dest = base_reg;
                    add_base.arch_src1 = base_reg;
                    add_base.imm = count * 4;
                    add_base.is_imm_valid = true;
                    uops.push_back(add_base);
                }
                break;
            }

            case Opcode::POP: {
                uint32_t count = 0;
                for (uint8_t r = 0; r < 16; ++r) {
                    if ((instr.register_list & (1U << r)) != 0) count++;
                }
                // Adjust SP first so that if POP contains PC (return), the updated SP is already renamed
                UOp add_sp;
                add_sp.seq_num = seq_num;
                add_sp.pc = pc;
                add_sp.type = UOpType::ALU;
                add_sp.target_port = ExecutionPort::PORT_0_ALU_BRANCH;
                add_sp.opcode = Opcode::ADD;
                add_sp.arch_dest = 13;
                add_sp.arch_src1 = 13;
                add_sp.imm = count * 4;
                add_sp.is_imm_valid = true;
                uops.push_back(add_sp);

                int32_t curr_offset = -static_cast<int32_t>(count * 4);
                for (uint8_t r = 0; r < 16; ++r) {
                    if ((instr.register_list & (1U << r)) != 0) {
                        UOp lda;
                        lda.seq_num = seq_num;
                        lda.pc = pc;
                        lda.type = (r == 15) ? UOpType::RET : UOpType::LOAD;
                        lda.target_port = ExecutionPort::PORT_2_LOAD_AGU;
                        lda.opcode = Opcode::LDR;
                        lda.arch_dest = r;
                        lda.arch_src1 = 13;
                        lda.imm = static_cast<uint32_t>(curr_offset);
                        lda.offset = curr_offset;
                        lda.is_imm_valid = true;
                        lda.mem_size_bytes = 4;
                        if (r == 15) {
                            lda.is_branch = true;
                        }
                        uops.push_back(lda);
                        curr_offset += 4;
                    }
                }
                break;
            }

            case Opcode::LDM: {
                uint8_t base_reg = instr.rn;
                uint32_t count = 0;
                for (uint8_t r = 0; r < 16; ++r) {
                    if ((instr.register_list & (1U << r)) != 0) count++;
                }
                if (instr.writeback) {
                    UOp add_base;
                    add_base.seq_num = seq_num;
                    add_base.pc = pc;
                    add_base.type = UOpType::ALU;
                    add_base.target_port = ExecutionPort::PORT_0_ALU_BRANCH;
                    add_base.opcode = Opcode::ADD;
                    add_base.arch_dest = base_reg;
                    add_base.arch_src1 = base_reg;
                    add_base.imm = count * 4;
                    add_base.is_imm_valid = true;
                    uops.push_back(add_base);
                }
                int32_t curr_offset = instr.writeback ? -static_cast<int32_t>(count * 4) : 0;
                for (uint8_t r = 0; r < 16; ++r) {
                    if ((instr.register_list & (1U << r)) != 0) {
                        UOp lda;
                        lda.seq_num = seq_num;
                        lda.pc = pc;
                        lda.type = (r == 15) ? UOpType::RET : UOpType::LOAD;
                        lda.target_port = ExecutionPort::PORT_2_LOAD_AGU;
                        lda.opcode = Opcode::LDR;
                        lda.arch_dest = r;
                        lda.arch_src1 = base_reg;
                        lda.imm = static_cast<uint32_t>(curr_offset);
                        lda.offset = curr_offset;
                        lda.is_imm_valid = true;
                        lda.mem_size_bytes = 4;
                        if (r == 15) {
                            lda.is_branch = true;
                        }
                        uops.push_back(lda);
                        curr_offset += 4;
                    }
                }
                break;
            }

            // -------------------------------------------------------------
            // Branches, Calls, Returns
            // -------------------------------------------------------------
            case Opcode::B:
            case Opcode::BX:
            case Opcode::BL:
            case Opcode::BLX:
            case Opcode::CBZ:
            case Opcode::CBNZ: {
                UOp uop;
                uop.seq_num = seq_num;
                uop.pc = pc;
                uop.raw_inst = instr.raw_hex;
                uop.is_thumb32 = (instr.instr_size == 4);
                uop.is_branch = true;
                uop.target_port = ExecutionPort::PORT_0_ALU_BRANCH;
                uop.opcode = instr.op;

                if (instr.op == Opcode::BL || instr.op == Opcode::BLX) {
                    uop.type = UOpType::CALL;
                    uop.arch_dest = 14; // LR = return PC
                    if (instr.op == Opcode::BLX) {
                        uop.arch_src1 = (instr.rm != 0) ? instr.rm : instr.rn;
                    }
                } else if (instr.op == Opcode::BX && (instr.rn == 14 || instr.rm == 14)) {
                    uop.type = UOpType::RET;
                    uop.arch_src1 = 14;
                } else {
                    uop.type = UOpType::BRANCH;
                    if (instr.op == Opcode::CBZ || instr.op == Opcode::CBNZ) {
                        uop.arch_src1 = instr.rn;
                    } else if (instr.op == Opcode::BX) {
                        uop.arch_src1 = (instr.rm != 0) ? instr.rm : instr.rn;
                    }
                }

                uop.cond = instr.cond;
                if (instr.is_relative) {
                    uop.actual_target = static_cast<uint32_t>(static_cast<int32_t>(pc + 4) + static_cast<int32_t>(instr.imm));
                } else {
                    uop.actual_target = instr.imm;
                }
                uop.imm = uop.actual_target;
                uop.offset = static_cast<int32_t>(instr.imm);
                uops.push_back(uop);
                break;
            }

            // -------------------------------------------------------------
            // Multiply Operations
            // -------------------------------------------------------------
            case Opcode::MUL:
            case Opcode::MLA: {
                UOp uop;
                uop.seq_num = seq_num;
                uop.pc = pc;
                uop.raw_inst = instr.raw_hex;
                uop.type = UOpType::MUL;
                uop.target_port = ExecutionPort::PORT_1_ALU_MUL;
                uop.opcode = instr.op;
                uop.arch_dest = instr.rd;
                uop.arch_src1 = instr.rn;
                uop.arch_src2 = instr.rm;
                uop.arch_src3 = instr.rs;
                uops.push_back(uop);
                break;
            }

            // -------------------------------------------------------------
            // System & NOP
            // -------------------------------------------------------------
            case Opcode::MRS:
            case Opcode::MSR: {
                UOp uop;
                uop.seq_num = seq_num;
                uop.pc = pc;
                uop.type = UOpType::SYS_REG;
                uop.target_port = ExecutionPort::PORT_0_ALU_BRANCH;
                uop.opcode = instr.op;
                uop.arch_dest = instr.rd;
                uop.arch_src1 = instr.rn;
                uops.push_back(uop);
                break;
            }
            case Opcode::SVC: {
                UOp uop;
                uop.seq_num = seq_num;
                uop.pc = pc;
                uop.type = UOpType::SVC;
                uop.target_port = ExecutionPort::PORT_0_ALU_BRANCH;
                uop.opcode = instr.op;
                uop.imm = instr.imm;
                uop.arch_src1 = 0; // R0
                uops.push_back(uop);
                break;
            }
            case Opcode::NOP: {
                UOp uop;
                uop.seq_num = seq_num;
                uop.pc = pc;
                uop.type = UOpType::NOP;
                uop.target_port = ExecutionPort::PORT_0_ALU_BRANCH;
                uop.opcode = instr.op;
                uops.push_back(uop);
                break;
            }
            case Opcode::UNKNOWN: {
                UOp uop;
                uop.seq_num = seq_num;
                uop.pc = pc;
                uop.type = UOpType::HALT;
                uop.opcode = instr.op;
                uops.push_back(uop);
                break;
            }

            // -------------------------------------------------------------
            // Standard ALU (ADD, SUB, MOV, CMP, AND, ORR, etc.)
            // -------------------------------------------------------------
            default: {
                UOp uop;
                uop.seq_num = seq_num;
                uop.pc = pc;
                uop.raw_inst = instr.raw_hex;
                uop.is_thumb32 = (instr.instr_size == 4);
                uop.type = UOpType::ALU;
                uop.target_port = ExecutionPort::PORT_0_ALU_BRANCH;
                uop.opcode = instr.op;
                uop.arch_dest = (instr.op == Opcode::CMP || instr.op == Opcode::CMN || instr.op == Opcode::TST || instr.op == Opcode::TEQ) ? UOp::INVALID_REG : instr.rd;
                uop.arch_src1 = instr.rn;
                uop.arch_src2 = instr.is_imm ? UOp::INVALID_REG : instr.rm;
                uop.imm = instr.imm;
                uop.offset = static_cast<int32_t>(instr.imm);
                uop.is_imm_valid = instr.is_imm;
                uop.sets_flags = instr.set_flags || (instr.op == Opcode::CMP || instr.op == Opcode::CMN || instr.op == Opcode::TST || instr.op == Opcode::TEQ);
                uop.cond = instr.cond;
                uops.push_back(uop);
                break;
            }
        }

        if (!uops.empty()) {
            for (size_t i = 0; i < uops.size() - 1; ++i) {
                uops[i].is_last_uop_of_macro_inst = false;
            }
            uops.back().is_last_uop_of_macro_inst = true;
        }

        return uops;
    }
};

} // namespace tinyarmsim::uarch
