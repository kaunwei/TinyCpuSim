#pragma once

#include <string>
#include <sstream>
#include <iomanip>
#include <cctype>
#include "tinyarmsim/cpu/instruction.hpp"

namespace tinyarmsim {

class Disassembler {
public:
    [[nodiscard]] static std::string reg_name(uint8_t r) {
        if (r == 13) return "sp";
        if (r == 14) return "lr";
        if (r == 15) return "pc";
        return "r" + std::to_string(r);
    }

    [[nodiscard]] static std::string cond_suffix(ConditionCode cond) {
        switch (cond) {
            case ConditionCode::EQ: return "eq";
            case ConditionCode::NE: return "ne";
            case ConditionCode::CS: return "cs";
            case ConditionCode::CC: return "cc";
            case ConditionCode::MI: return "mi";
            case ConditionCode::PL: return "pl";
            case ConditionCode::VS: return "vs";
            case ConditionCode::VC: return "vc";
            case ConditionCode::HI: return "hi";
            case ConditionCode::LS: return "ls";
            case ConditionCode::GE: return "ge";
            case ConditionCode::LT: return "lt";
            case ConditionCode::GT: return "gt";
            case ConditionCode::LE: return "le";
            case ConditionCode::AL: return "";
            case ConditionCode::NV: return "nv";
        }
        return "";
    }

    [[nodiscard]] static std::string reg_list_to_string(uint16_t rlist) {
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (int i = 0; i < 16; ++i) {
            if (rlist & (1 << i)) {
                if (!first) oss << ", ";
                oss << reg_name(static_cast<uint8_t>(i));
                first = false;
            }
        }
        oss << "}";
        return oss.str();
    }

    [[nodiscard]] static std::string disassemble(const DecodedInstruction& instr) {
        std::ostringstream oss;
        std::string op_str = std::string(opcode_to_string(instr.op));
        for (auto& c : op_str) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        std::string s_flag = (instr.set_flags && instr.op != Opcode::CMP && instr.op != Opcode::CMN && 
                              instr.op != Opcode::TST && instr.op != Opcode::TEQ) ? "s" : "";
        std::string cond_str = cond_suffix(instr.cond);

        switch (instr.op) {
            case Opcode::MOV:
            case Opcode::MVN: {
                oss << op_str << s_flag << cond_str << " " << reg_name(instr.rd) << ", ";
                if (instr.is_imm) oss << "#" << instr.imm;
                else oss << reg_name(instr.rm);
                break;
            }

            case Opcode::MOVW:
            case Opcode::MOVT: {
                oss << op_str << cond_str << " " << reg_name(instr.rd) << ", #0x"
                    << std::hex << instr.imm << std::dec;
                break;
            }

            case Opcode::ADD:
            case Opcode::ADC:
            case Opcode::SUB:
            case Opcode::SBC:
            case Opcode::RSB:
            case Opcode::AND:
            case Opcode::ORR:
            case Opcode::EOR:
            case Opcode::BIC: {
                oss << op_str << s_flag << cond_str << " " << reg_name(instr.rd) << ", "
                    << reg_name(instr.rn) << ", ";
                if (instr.is_imm) oss << "#" << instr.imm;
                else oss << reg_name(instr.rm);
                break;
            }

            case Opcode::CMP:
            case Opcode::CMN:
            case Opcode::TST:
            case Opcode::TEQ: {
                oss << op_str << cond_str << " " << reg_name(instr.rn) << ", ";
                if (instr.is_imm) oss << "#" << instr.imm;
                else oss << reg_name(instr.rm);
                break;
            }

            case Opcode::MUL: {
                oss << op_str << s_flag << cond_str << " " << reg_name(instr.rd) << ", "
                    << reg_name(instr.rn) << ", " << reg_name(instr.rm);
                break;
            }

            case Opcode::MLA: {
                oss << op_str << cond_str << " " << reg_name(instr.rd) << ", "
                    << reg_name(instr.rn) << ", " << reg_name(instr.rm) << ", "
                    << reg_name(instr.rs);
                break;
            }

            case Opcode::ASR:
            case Opcode::LSL:
            case Opcode::LSR:
            case Opcode::ROR: {
                oss << op_str << s_flag << cond_str << " " << reg_name(instr.rd) << ", "
                    << reg_name(instr.rn) << ", ";
                if (instr.is_imm) oss << "#" << instr.imm;
                else oss << reg_name(instr.rm);
                break;
            }

            case Opcode::B: {
                oss << "b" << cond_str << " 0x" << std::hex << instr.imm << std::dec;
                break;
            }

            case Opcode::BL:
            case Opcode::BLX: {
                oss << op_str << cond_str << " ";
                if (instr.is_imm) oss << "0x" << std::hex << instr.imm << std::dec;
                else oss << reg_name(instr.rm);
                break;
            }

            case Opcode::BX: {
                oss << "bx" << cond_str << " " << reg_name(instr.rm);
                break;
            }

            case Opcode::CBZ:
            case Opcode::CBNZ: {
                oss << op_str << " " << reg_name(instr.rn) << ", 0x"
                    << std::hex << instr.imm << std::dec;
                break;
            }

            case Opcode::LDR:
            case Opcode::LDRB:
            case Opcode::LDRH:
            case Opcode::LDRSB:
            case Opcode::LDRSH:
            case Opcode::STR:
            case Opcode::STRB:
            case Opcode::STRH: {
                oss << op_str << cond_str << " " << reg_name(instr.rd) << ", ["
                    << reg_name(instr.rn);
                if (instr.is_imm) {
                    if (instr.imm != 0) oss << ", #" << instr.imm;
                } else {
                    oss << ", " << reg_name(instr.rm);
                }
                oss << "]";
                break;
            }

            case Opcode::STM:
            case Opcode::LDM: {
                oss << op_str << "ia "
                    << reg_name(instr.rn) << (instr.writeback ? "! " : " ")
                    << reg_list_to_string(instr.register_list);
                break;
            }

            case Opcode::PUSH:
            case Opcode::POP: {
                oss << op_str << " " << reg_list_to_string(instr.register_list);
                break;
            }

            case Opcode::MRS: {
                oss << "mrs " << reg_name(instr.rd) << ", cpsr";
                break;
            }

            case Opcode::MSR: {
                oss << "msr cpsr_f, " << reg_name(instr.rn);
                break;
            }

            case Opcode::SXTB:
            case Opcode::SXTH:
            case Opcode::UXTB:
            case Opcode::UXTH:
            case Opcode::REV:
            case Opcode::REV16:
            case Opcode::REVSH: {
                oss << op_str << " " << reg_name(instr.rd) << ", " << reg_name(instr.rm);
                break;
            }
            case Opcode::UDIV:
            case Opcode::SDIV: {
                oss << op_str << " " << reg_name(instr.rd) << ", " << reg_name(instr.rn) << ", " << reg_name(instr.rm);
                break;
            }
            case Opcode::ADR: {
                oss << "adr " << reg_name(instr.rd) << ", #" << instr.imm;
                break;
            }
            case Opcode::CPSIE: oss << "cpsie i"; break;
            case Opcode::CPSID: oss << "cpsid i"; break;
            case Opcode::WFI:   oss << "wfi"; break;
            case Opcode::DMB:   oss << "dmb"; break;
            case Opcode::DSB:   oss << "dsb"; break;
            case Opcode::ISB:   oss << "isb"; break;

            case Opcode::NOP: {
                oss << "nop";
                break;
            }

            case Opcode::SVC: {
                oss << "svc #" << instr.imm;
                break;
            }

            default:
                oss << "unknown";
                break;
        }

        return oss.str();
    }
};

} // namespace tinyarmsim
