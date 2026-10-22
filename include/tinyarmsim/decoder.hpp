#pragma once

#include <cstdint>
#include "tinyarmsim/instruction.hpp"
#include "tinyarmsim/disassembler.hpp"
#include "tinyarmsim/faults.hpp"

namespace tinyarmsim {

class Decoder {
public:
    [[nodiscard]] static bool is_32bit_thumb(uint16_t first_halfword) noexcept {
        uint16_t top5 = (first_halfword >> 11) & 0x1F;
        return (top5 == 0b11101 || top5 == 0b11110 || top5 == 0b11111);
    }

    [[nodiscard]] static DecodedInstruction decode16(uint16_t raw_instr, uint32_t pc = 0) {
        DecodedInstruction instr = decode16_internal(raw_instr, pc);
        instr.raw_hex = raw_instr;
        instr.disasm = Disassembler::disassemble(instr);
        return instr;
    }

    [[nodiscard]] static DecodedInstruction decode32(uint16_t w1, uint16_t w2, uint32_t pc = 0) {
        DecodedInstruction instr = decode32_internal(w1, w2, pc);
        instr.raw_hex = (static_cast<uint32_t>(w1) << 16) | w2;
        instr.disasm = Disassembler::disassemble(instr);
        return instr;
    }

private:
    [[nodiscard]] static DecodedInstruction decode16_internal(uint16_t raw_instr, uint32_t pc) {
        DecodedInstruction instr{};
        instr.instr_size = 2;
        instr.cond = ConditionCode::AL;

        // 1. Shift by immediate: [15:13] == 000 (0x0000 - 0x1FFF)
        if ((raw_instr & 0xE000) == 0x0000) {
            uint8_t op5 = static_cast<uint8_t>((raw_instr >> 11) & 0x3);
            uint8_t imm5 = static_cast<uint8_t>((raw_instr >> 6) & 0x1F);
            uint8_t rm = static_cast<uint8_t>((raw_instr >> 3) & 0x7);
            uint8_t rd = static_cast<uint8_t>(raw_instr & 0x7);

            if (op5 != 0b11) {
                instr.rd = rd;
                instr.set_flags = true;

                if (op5 == 0b00 && imm5 == 0) {
                    instr.op = Opcode::MOV;
                    instr.rn = rm;
                    instr.rm = rm;
                    instr.is_imm = false;
                } else {
                    instr.rn = rm;
                    instr.is_imm = true;
                    instr.imm = imm5;
                    if (op5 == 0b00) {
                        instr.op = Opcode::LSL;
                    } else if (op5 == 0b01) {
                        instr.op = Opcode::LSR;
                    } else if (op5 == 0b10) {
                        instr.op = Opcode::ASR;
                    }
                }
                return instr;
            }
        }

        // 2. Add/subtract register / 3-bit immediate: [15:11] == 00011 (0x1800-0x1FFF)
        if ((raw_instr & 0xF800) == 0x1800) {
            uint8_t op_bit = static_cast<uint8_t>((raw_instr >> 9) & 0x1);
            bool is_imm3 = ((raw_instr >> 10) & 0x1) != 0;
            instr.rd = static_cast<uint8_t>(raw_instr & 0x7);
            instr.rn = static_cast<uint8_t>((raw_instr >> 3) & 0x7);
            instr.set_flags = true;
            instr.op = (op_bit == 0) ? Opcode::ADD : Opcode::SUB;

            if (is_imm3) {
                instr.is_imm = true;
                instr.imm = static_cast<uint32_t>((raw_instr >> 6) & 0x7);
            } else {
                instr.is_imm = false;
                instr.rm = static_cast<uint8_t>((raw_instr >> 6) & 0x7);
            }
            return instr;
        }

        // 3. Move, compare, add, subtract 8-bit immediate: (0x2000-0x3FFF)
        if ((raw_instr & 0xF800) == 0x2000) { // MOVS Rd, #imm8
            instr.op = Opcode::MOV;
            instr.rd = static_cast<uint8_t>((raw_instr >> 8) & 0x7);
            instr.is_imm = true;
            instr.imm = raw_instr & 0xFF;
            instr.set_flags = true;
            return instr;
        }

        if ((raw_instr & 0xF800) == 0x2800) { // CMP Rn, #imm8
            instr.op = Opcode::CMP;
            instr.rn = static_cast<uint8_t>((raw_instr >> 8) & 0x7);
            instr.is_imm = true;
            instr.imm = raw_instr & 0xFF;
            instr.set_flags = true;
            return instr;
        }

        if ((raw_instr & 0xF800) == 0x3000) { // ADDS Rd, #imm8
            instr.op = Opcode::ADD;
            instr.rd = static_cast<uint8_t>((raw_instr >> 8) & 0x7);
            instr.rn = instr.rd;
            instr.is_imm = true;
            instr.imm = raw_instr & 0xFF;
            instr.set_flags = true;
            return instr;
        }

        if ((raw_instr & 0xF800) == 0x3800) { // SUBS Rd, #imm8
            instr.op = Opcode::SUB;
            instr.rd = static_cast<uint8_t>((raw_instr >> 8) & 0x7);
            instr.rn = instr.rd;
            instr.is_imm = true;
            instr.imm = raw_instr & 0xFF;
            instr.set_flags = true;
            return instr;
        }

        // 4. ALU operations: [15:10] == 010000 (0x4000-0x43FF)
        if ((raw_instr & 0xFC00) == 0x4000) {
            uint8_t alu_op = static_cast<uint8_t>((raw_instr >> 6) & 0xF);
            instr.rd = static_cast<uint8_t>(raw_instr & 0x7);
            instr.rn = instr.rd;
            instr.rm = static_cast<uint8_t>((raw_instr >> 3) & 0x7);
            instr.is_imm = false;
            instr.set_flags = true;

            switch (alu_op) {
                case 0b0000: instr.op = Opcode::AND; break;
                case 0b0001: instr.op = Opcode::EOR; break;
                case 0b0010: instr.op = Opcode::LSL; break;
                case 0b0011: instr.op = Opcode::LSR; break;
                case 0b0100: instr.op = Opcode::ASR; break;
                case 0b0101: instr.op = Opcode::ADC; break;
                case 0b0110: instr.op = Opcode::SBC; break;
                case 0b0111: instr.op = Opcode::ROR; break;
                case 0b1000: instr.op = Opcode::TST; break;
                case 0b1001: instr.op = Opcode::RSB; break;
                case 0b1010: instr.op = Opcode::CMP; break;
                case 0b1011: instr.op = Opcode::CMN; break;
                case 0b1100: instr.op = Opcode::ORR; break;
                case 0b1101: instr.op = Opcode::MUL; break;
                case 0b1110: instr.op = Opcode::BIC; break;
                case 0b1111: instr.op = Opcode::MVN; break;
            }
            return instr;
        }

        // 5. Special data / branch exchange (0x4400-0x47FF)
        if ((raw_instr & 0xFC00) == 0x4400) {
            uint8_t sub_op = static_cast<uint8_t>((raw_instr >> 8) & 0x3);
            uint8_t rd = static_cast<uint8_t>((raw_instr & 0x7) | ((raw_instr >> 4) & 0x8));
            uint8_t rm = static_cast<uint8_t>((raw_instr >> 3) & 0xF);

            if (sub_op == 0b00) {
                instr.op = Opcode::ADD;
                instr.rd = rd; instr.rn = rd; instr.rm = rm;
                instr.set_flags = false;
                return instr;
            } else if (sub_op == 0b01) {
                instr.op = Opcode::CMP;
                instr.rn = rd; instr.rm = rm;
                instr.set_flags = true;
                return instr;
            } else if (sub_op == 0b10) {
                instr.op = Opcode::MOV;
                instr.rd = rd; instr.rm = rm;
                instr.set_flags = false;
                return instr;
            } else if (sub_op == 0b11) {
                instr.op = ((raw_instr & 0x0080) == 0) ? Opcode::BX : Opcode::BLX;
                instr.rm = rm;
                return instr;
            }
        }

        // 5.5 Load literal (PC-relative load): [15:11] == 01001 (0x4800 - 0x4FFF)
        if ((raw_instr & 0xF800) == 0x4800) {
            uint8_t rd = static_cast<uint8_t>((raw_instr >> 8) & 0x7);
            uint32_t imm8 = raw_instr & 0xFF;
            instr.op = Opcode::LDR;
            instr.rd = rd;
            instr.rn = 15; // PC
            instr.is_imm = true;
            instr.imm = imm8 * 4;
            instr.mem_size = 4;
            return instr;
        }

        // 6. Load/Store register offset: [15:12] == 0101 (0x5000 - 0x5FFF)
        if ((raw_instr & 0xF000) == 0x5000) {
            uint8_t op3 = static_cast<uint8_t>((raw_instr >> 9) & 0x7);
            uint8_t rm = static_cast<uint8_t>((raw_instr >> 6) & 0x7);
            uint8_t rn = static_cast<uint8_t>((raw_instr >> 3) & 0x7);
            uint8_t rd = static_cast<uint8_t>(raw_instr & 0x7);

            instr.rd = rd;
            instr.rn = rn;
            instr.rm = rm;
            instr.is_imm = false;

            switch (op3) {
                case 0b000: instr.op = Opcode::STR; instr.mem_size = 4; break;
                case 0b001: instr.op = Opcode::STRH; instr.mem_size = 2; break;
                case 0b010: instr.op = Opcode::STRB; instr.mem_size = 1; break;
                case 0b011: instr.op = Opcode::LDRSB; instr.mem_size = 1; instr.is_signed_mem = true; break;
                case 0b100: instr.op = Opcode::LDR; instr.mem_size = 4; break;
                case 0b101: instr.op = Opcode::LDRH; instr.mem_size = 2; break;
                case 0b110: instr.op = Opcode::LDRB; instr.mem_size = 1; break;
                case 0b111: instr.op = Opcode::LDRSH; instr.mem_size = 2; instr.is_signed_mem = true; break;
            }
            return instr;
        }

        // 7. Load/Store word immediate offset: [15:12] == 0110 (0x6000 - 0x6FFF)
        if ((raw_instr & 0xF000) == 0x6000) {
            bool is_load = ((raw_instr >> 11) & 0x1) != 0;
            uint32_t imm5 = (raw_instr >> 6) & 0x1F;
            uint8_t rn = static_cast<uint8_t>((raw_instr >> 3) & 0x7);
            uint8_t rd = static_cast<uint8_t>(raw_instr & 0x7);

            instr.op = is_load ? Opcode::LDR : Opcode::STR;
            instr.rd = rd;
            instr.rn = rn;
            instr.is_imm = true;
            instr.imm = imm5 * 4;
            instr.mem_size = 4;
            return instr;
        }

        // 8. Load/Store byte immediate offset: [15:12] == 0111 (0x7000 - 0x7FFF)
        if ((raw_instr & 0xF000) == 0x7000) {
            bool is_load = ((raw_instr >> 11) & 0x1) != 0;
            uint32_t imm5 = (raw_instr >> 6) & 0x1F;
            uint8_t rn = static_cast<uint8_t>((raw_instr >> 3) & 0x7);
            uint8_t rd = static_cast<uint8_t>(raw_instr & 0x7);

            instr.op = is_load ? Opcode::LDRB : Opcode::STRB;
            instr.rd = rd;
            instr.rn = rn;
            instr.is_imm = true;
            instr.imm = imm5;
            instr.mem_size = 1;
            return instr;
        }

        // 9. Load/Store halfword immediate offset: [15:12] == 1000 (0x8000 - 0x8FFF)
        if ((raw_instr & 0xF000) == 0x8000) {
            bool is_load = ((raw_instr >> 11) & 0x1) != 0;
            uint32_t imm5 = (raw_instr >> 6) & 0x1F;
            uint8_t rn = static_cast<uint8_t>((raw_instr >> 3) & 0x7);
            uint8_t rd = static_cast<uint8_t>(raw_instr & 0x7);

            instr.op = is_load ? Opcode::LDRH : Opcode::STRH;
            instr.rd = rd;
            instr.rn = rn;
            instr.is_imm = true;
            instr.imm = imm5 * 2;
            instr.mem_size = 2;
            return instr;
        }

        // 10. Load/Store SP-relative: [15:12] == 1001 (0x9000 - 0x9FFF)
        if ((raw_instr & 0xF000) == 0x9000) {
            bool is_load = ((raw_instr >> 11) & 0x1) != 0;
            uint8_t rd = static_cast<uint8_t>((raw_instr >> 8) & 0x7);
            uint32_t imm8 = raw_instr & 0xFF;

            instr.op = is_load ? Opcode::LDR : Opcode::STR;
            instr.rd = rd;
            instr.rn = 13; // SP
            instr.is_imm = true;
            instr.imm = imm8 * 4;
            instr.mem_size = 4;
            return instr;
        }

        // 11. PUSH / POP: [15:12] == 1011, [10:9] == 10 (0xB400 / 0xBC00)
        if ((raw_instr & 0xF600) == 0xB400) {
            bool is_pop = ((raw_instr >> 11) & 0x1) != 0;
            bool extra_reg = ((raw_instr >> 8) & 0x1) != 0;
            uint16_t rlist = raw_instr & 0xFF;

            if (is_pop) {
                instr.op = Opcode::POP;
                if (extra_reg) rlist |= (1 << 15);
            } else {
                instr.op = Opcode::PUSH;
                if (extra_reg) rlist |= (1 << 14);
            }
            instr.register_list = rlist;
            return instr;
        }

        // 12. Multiple Load/Store (LDM/STM): [15:12] == 1100 (0xC000 - 0xCFFF)
        if ((raw_instr & 0xF000) == 0xC000) {
            bool is_load = ((raw_instr >> 11) & 0x1) != 0;
            uint8_t rn = static_cast<uint8_t>((raw_instr >> 8) & 0x7);
            uint16_t rlist = raw_instr & 0xFF;

            instr.op = is_load ? Opcode::LDM : Opcode::STM;
            instr.rn = rn;
            instr.register_list = rlist;
            instr.writeback = true;
            return instr;
        }

        // 13. CBZ / CBNZ: [15:12] == 1011, [10:9] == 00/01 (0xB100 / 0xB900)
        if ((raw_instr & 0xF500) == 0xB100) {
            bool is_cbnz = ((raw_instr >> 11) & 0x1) != 0;
            instr.op = is_cbnz ? Opcode::CBNZ : Opcode::CBZ;
            instr.rn = static_cast<uint8_t>(raw_instr & 0x7);
            uint32_t i = (raw_instr >> 9) & 0x1;
            uint32_t imm5 = (raw_instr >> 3) & 0x1F;
            uint32_t offset = ((i << 5) | imm5) << 1;
            instr.is_imm = true;
            instr.is_relative = (pc == 0);
            instr.imm = (pc != 0) ? (pc + 4 + offset) : offset;
            return instr;
        }

        // 14. NOP (0xBF00)
        if (raw_instr == 0xBF00) {
            instr.op = Opcode::NOP;
            return instr;
        }

        // 14.1 System Hints: WFE (0xBF20), WFI (0xBF30), SEV (0xBF40), IT (0xBF01-0xBFEF)
        if ((raw_instr & 0xFF00) == 0xBF00) {
            if (raw_instr == 0xBF30 || raw_instr == 0xBF20) {
                instr.op = Opcode::WFI;
            } else {
                instr.op = Opcode::NOP;
            }
            return instr;
        }

        // 14.2 Form PC/SP-relative address: ADR Rd, #imm8 (0xA000) / ADD Rd, SP, #imm8 (0xA800)
        if ((raw_instr & 0xF000) == 0xA000) {
            bool from_sp = ((raw_instr >> 11) & 0x1) != 0;
            uint8_t rd = static_cast<uint8_t>((raw_instr >> 8) & 0x7);
            uint32_t imm8 = (raw_instr & 0xFFu) * 4u;
            instr.rd = rd;
            instr.is_imm = true;
            instr.imm = imm8;
            if (from_sp) {
                instr.op = Opcode::ADD;
                instr.rn = 13; // SP
            } else {
                instr.op = Opcode::ADR;
                instr.rn = 15; // PC
            }
            return instr;
        }

        // 14.3 Adjust SP: ADD SP, SP, #imm7 (0xB000) / SUB SP, SP, #imm7 (0xB080)
        if ((raw_instr & 0xFF00) == 0xB000) {
            bool is_sub = ((raw_instr >> 7) & 0x1) != 0;
            instr.op = is_sub ? Opcode::SUB : Opcode::ADD;
            instr.rd = 13;
            instr.rn = 13;
            instr.is_imm = true;
            instr.imm = (raw_instr & 0x7Fu) * 4u;
            instr.set_flags = false;
            return instr;
        }

        // 14.4 Sign/Zero Extend: SXTH (0xB200), SXTB (0xB240), UXTH (0xB280), UXTB (0xB2C0)
        if ((raw_instr & 0xFF00) == 0xB200) {
            uint8_t op2 = static_cast<uint8_t>((raw_instr >> 6) & 0x3);
            instr.rm = static_cast<uint8_t>((raw_instr >> 3) & 0x7);
            instr.rd = static_cast<uint8_t>(raw_instr & 0x7);
            switch (op2) {
                case 0b00: instr.op = Opcode::SXTH; break;
                case 0b01: instr.op = Opcode::SXTB; break;
                case 0b10: instr.op = Opcode::UXTH; break;
                case 0b11: instr.op = Opcode::UXTB; break;
            }
            return instr;
        }

        // 14.5 Byte Reverse: REV (0xBA00), REV16 (0xBA40), REVSH (0xBAC0)
        if ((raw_instr & 0xFF00) == 0xBA00) {
            uint8_t op2 = static_cast<uint8_t>((raw_instr >> 6) & 0x3);
            instr.rm = static_cast<uint8_t>((raw_instr >> 3) & 0x7);
            instr.rd = static_cast<uint8_t>(raw_instr & 0x7);
            if (op2 == 0b00) instr.op = Opcode::REV;
            else if (op2 == 0b01) instr.op = Opcode::REV16;
            else if (op2 == 0b11) instr.op = Opcode::REVSH;
            else throw UndefinedInstructionException(raw_instr, pc);
            return instr;
        }

        // 14.6 CPSIE / CPSID (0xB660 - 0xB67F)
        if ((raw_instr & 0xFFE0) == 0xB660) {
            bool disable = ((raw_instr >> 4) & 0x1) != 0;
            instr.op = disable ? Opcode::CPSID : Opcode::CPSIE;
            return instr;
        }

        // 15. Software Interrupt (SVC #imm8): [15:8] == 0xDF
        if ((raw_instr & 0xFF00) == 0xDF00) {
            instr.op = Opcode::SVC;
            instr.is_imm = true;
            instr.imm = raw_instr & 0xFF;
            return instr;
        }

        // 16. Conditional Branch (B<cond> #imm8): [15:12] == 1101 (0xD000 - 0xDEFF)
        if ((raw_instr & 0xF000) == 0xD000) {
            uint8_t cond_val = static_cast<uint8_t>((raw_instr >> 8) & 0xF);
            if (cond_val < 0xE) {
                instr.op = Opcode::B;
                instr.cond = static_cast<ConditionCode>(cond_val);
                int32_t imm8_signed = static_cast<int8_t>(raw_instr & 0xFF);
                int32_t offset = imm8_signed * 2;
                instr.is_imm = true;
                instr.is_relative = (pc == 0);
                instr.imm = static_cast<uint32_t>((pc != 0) ? (static_cast<int32_t>(pc + 4) + offset) : offset);
                return instr;
            }
        }

        // 17. Unconditional Branch (B #imm11): [15:11] == 11100 (0xE000 - 0xE7FF)
        if ((raw_instr & 0xF800) == 0xE000) {
            instr.op = Opcode::B;
            instr.cond = ConditionCode::AL;
            int32_t imm11 = raw_instr & 0x7FF;
            if (imm11 & 0x400) {
                imm11 |= ~0x7FF;
            }
            int32_t offset = imm11 * 2;
            instr.is_imm = true;
            instr.is_relative = (pc == 0);
            instr.imm = static_cast<uint32_t>((pc != 0) ? (static_cast<int32_t>(pc + 4) + offset) : offset);
            return instr;
        }

        throw UndefinedInstructionException(raw_instr, pc);
    }

    [[nodiscard]] static DecodedInstruction decode32_internal(uint16_t w1, uint16_t w2, uint32_t pc = 0) {
        DecodedInstruction instr{};
        instr.instr_size = 4;
        instr.cond = ConditionCode::AL;

        // 1. MOVW / MOVT:
        if ((w1 & 0xFB70) == 0xF240) {
            bool is_movt = ((w1 >> 7) & 0x1) != 0;
            instr.op = is_movt ? Opcode::MOVT : Opcode::MOVW;

            uint32_t imm4 = w1 & 0xF;
            uint32_t i = (w1 >> 10) & 0x1;
            uint32_t imm3 = (w2 >> 12) & 0x7;
            uint32_t rd = (w2 >> 8) & 0xF;
            uint32_t imm8 = w2 & 0xFF;

            uint32_t imm16 = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
            instr.rd = static_cast<uint8_t>(rd);
            instr.rn = static_cast<uint8_t>(rd);
            instr.is_imm = true;
            instr.imm = imm16;
            return instr;
        }

        // 2. 32-bit Data Processing (Modified Immediate): [15:11] == 11110, bit 9 == 0
        if ((w1 & 0xFBE0) == 0xF000 && (w2 & 0x8000) == 0) {
            uint8_t op4 = static_cast<uint8_t>((w1 >> 5) & 0xF);
            bool set_flags = ((w1 >> 4) & 0x1) != 0;
            uint8_t rn = static_cast<uint8_t>(w1 & 0xF);
            uint32_t i = (w1 >> 10) & 0x1;
            uint32_t imm3 = (w2 >> 12) & 0x7;
            uint8_t rd = static_cast<uint8_t>((w2 >> 8) & 0xF);
            uint32_t imm8 = w2 & 0xFF;
            uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;

            uint32_t imm_val = 0;
            uint32_t type = (imm12 >> 8) & 0xF;
            uint32_t val8 = imm12 & 0xFF;
            if ((type >> 2) == 0) {
                switch (type & 0x3) {
                    case 0b00: imm_val = val8; break;
                    case 0b01: imm_val = (val8 << 16) | val8; break;
                    case 0b10: imm_val = (val8 << 24) | (val8 << 8); break;
                    case 0b11: imm_val = (val8 << 24) | (val8 << 16) | (val8 << 8) | val8; break;
                }
            } else {
                uint32_t unrotated = (1u << 7) | (imm12 & 0x7F);
                uint32_t rot = (imm12 >> 7) & 0x1F;
                imm_val = (unrotated >> rot) | (unrotated << (32 - rot));
            }

            instr.rd = rd;
            instr.rn = rn;
            instr.is_imm = true;
            instr.imm = imm_val;
            instr.set_flags = set_flags;

            switch (op4) {
                case 0b0000: instr.op = (rd == 0xF) ? Opcode::TST : Opcode::AND; break;
                case 0b0001: instr.op = Opcode::BIC; break;
                case 0b0010: instr.op = (rn == 0xF) ? Opcode::MOV : Opcode::ORR; break;
                case 0b0011: instr.op = (rn == 0xF) ? Opcode::MVN : Opcode::ORR; break;
                case 0b0100: instr.op = (rd == 0xF) ? Opcode::TEQ : Opcode::EOR; break;
                case 0b1000: instr.op = (rd == 0xF) ? Opcode::CMN : Opcode::ADD; break;
                case 0b1010: instr.op = Opcode::ADC; break;
                case 0b1011: instr.op = Opcode::SBC; break;
                case 0b1101: instr.op = (rd == 0xF) ? Opcode::CMP : Opcode::SUB; break;
                case 0b1110: instr.op = Opcode::RSB; break;
                default: instr.op = Opcode::UNKNOWN; break;
            }
            if (instr.op != Opcode::UNKNOWN) {
                return instr;
            }
        }

        // 2. 32-bit Data Processing (modified register / shifted): [15:9] == 1110101 (0xEA00/0xEB00)
        if ((w1 & 0xFE00) == 0xEA00) {
            uint8_t op4 = static_cast<uint8_t>((w1 >> 5) & 0xF);
            bool set_flags = ((w1 >> 4) & 0x1) != 0;
            uint8_t rn = static_cast<uint8_t>(w1 & 0xF);
            uint8_t rd = static_cast<uint8_t>((w2 >> 8) & 0xF);
            uint8_t rm = static_cast<uint8_t>(w2 & 0xF);

            instr.rd = rd;
            instr.rn = rn;
            instr.rm = rm;
            instr.set_flags = set_flags;

            switch (op4) {
                case 0b0000: instr.op = Opcode::AND; break;
                case 0b0001: instr.op = Opcode::BIC; break;
                case 0b0010: instr.op = (rn == 0xF) ? Opcode::MOV : Opcode::ORR; break;
                case 0b0011: instr.op = (rn == 0xF) ? Opcode::MVN : Opcode::ORR; break;
                case 0b0100: instr.op = (rd == 0xF) ? Opcode::TEQ : Opcode::EOR; break;
                case 0b1000: instr.op = (rd == 0xF) ? Opcode::CMN : Opcode::ADD; break;
                case 0b1010: instr.op = Opcode::ADC; break;
                case 0b1011: instr.op = Opcode::SBC; break;
                case 0b1101: instr.op = (rd == 0xF) ? Opcode::CMP : Opcode::SUB; break;
                case 0b1110: instr.op = Opcode::RSB; break;
                default: instr.op = Opcode::UNKNOWN; break;
            }
            if (instr.op != Opcode::UNKNOWN) {
                return instr;
            }
        }

        // 3. 32-bit Multiply and Multiply Accumulate (MUL / MLA):
        if ((w1 & 0xFFF0) == 0xFB00 && (w2 & 0x00F0) == 0x0000) {
            uint8_t rn = static_cast<uint8_t>(w1 & 0xF);
            uint8_t ra = static_cast<uint8_t>((w2 >> 12) & 0xF);
            uint8_t rd = static_cast<uint8_t>((w2 >> 8) & 0xF);
            uint8_t rm = static_cast<uint8_t>(w2 & 0xF);

            if (ra == 0xF) {
                instr.op = Opcode::MUL;
                instr.rd = rd; instr.rn = rn; instr.rm = rm;
            } else {
                instr.op = Opcode::MLA;
                instr.rd = rd; instr.rn = rn; instr.rm = rm; instr.rs = ra;
            }
            return instr;
        }

        // 4. 32-bit System instructions (MRS / MSR):
        if (w1 == 0xF3EF && (w2 & 0xF000) == 0x8000) {
            instr.op = Opcode::MRS;
            instr.rd = static_cast<uint8_t>((w2 >> 8) & 0xF);
            return instr;
        }

        if ((w1 & 0xFFF0) == 0xF380 && (w2 & 0xFF00) == 0x8800) {
            instr.op = Opcode::MSR;
            instr.rn = static_cast<uint8_t>(w1 & 0xF);
            return instr;
        }

        // 5. Branch with Link (BL / BLX):
        if ((w1 & 0xF800) == 0xF000 && (w2 & 0xD000) == 0xD000) {
            bool is_blx = ((w2 & 0x1000) == 0);
            instr.op = is_blx ? Opcode::BLX : Opcode::BL;

            uint32_t s = (w1 >> 10) & 0x1;
            uint32_t imm10 = w1 & 0x3FF;
            uint32_t j1 = (w2 >> 13) & 0x1;
            uint32_t j2 = (w2 >> 11) & 0x1;
            uint32_t imm11 = w2 & 0x7FF;

            uint32_t i1 = (~(j1 ^ s)) & 0x1;
            uint32_t i2 = (~(j2 ^ s)) & 0x1;

            int32_t offset = static_cast<int32_t>((s ? 0xFF000000u : 0u) |
                                                  (i1 << 23) | (i2 << 22) |
                                                  (imm10 << 12) | (imm11 << 1));
            instr.is_imm = true;
            instr.is_relative = (pc == 0);
            instr.imm = static_cast<uint32_t>((pc != 0) ? (static_cast<int32_t>(pc + 4) + offset) : offset);
            return instr;
        }

        // 6. 32-bit Load/Store (LDR/STR.W):
        if ((w1 & 0xFE00) == 0xF800) {
            bool is_load = ((w1 >> 4) & 0x1) != 0;
            uint8_t size_code = static_cast<uint8_t>((w1 >> 5) & 0x3);
            uint8_t rn = static_cast<uint8_t>(w1 & 0xF);
            uint8_t rd = static_cast<uint8_t>((w2 >> 12) & 0xF);
            uint32_t imm12 = w2 & 0xFFF;

            instr.rd = rd;
            instr.rn = rn;
            instr.is_imm = true;
            instr.imm = imm12;

            if (size_code == 0b00) {
                instr.op = is_load ? Opcode::LDRB : Opcode::STRB;
                instr.mem_size = 1;
            } else if (size_code == 0b01) {
                instr.op = is_load ? Opcode::LDRH : Opcode::STRH;
                instr.mem_size = 2;
            } else if (size_code == 0b10) {
                instr.op = is_load ? Opcode::LDR : Opcode::STR;
                instr.mem_size = 4;
            }
            return instr;
        }

        uint32_t raw32 = (static_cast<uint32_t>(w1) << 16) | w2;
        throw UndefinedInstructionException(raw32, pc);
    }
};

} // namespace tinyarmsim
