#pragma once

#include <cstdint>
#include <string_view>
#include <string>

namespace tinyarmsim {

enum class Opcode {
    UNKNOWN = 0,
    // Data processing
    MOV,
    MVN,
    MOVT,
    MOVW,
    ADD,
    ADC,
    SUB,
    SBC,
    RSB,
    MUL,
    MLA,
    AND,
    ORR,
    EOR,
    BIC,
    CMP,
    CMN,
    TST,
    TEQ,
    ASR,
    LSL,
    LSR,
    ROR,
    // Branching
    B,
    BL,
    BX,
    BLX,
    CBZ,
    CBNZ,
    // Memory access
    LDR,
    LDRB,
    LDRH,
    LDRSB,
    LDRSH,
    STR,
    STRB,
    STRH,
    LDM,
    STM,
    PUSH,
    POP,
    // Sign/Zero Extend & Byte Reverse & Divide
    SXTB,
    SXTH,
    UXTB,
    UXTH,
    REV,
    REV16,
    REVSH,
    UDIV,
    SDIV,
    ADR,
    // System & Barriers
    CPSIE,
    CPSID,
    WFI,
    DMB,
    DSB,
    ISB,
    // System
    MRS,
    MSR,
    SVC,
    NOP
};

[[nodiscard]] constexpr std::string_view opcode_to_string(Opcode op) noexcept {
    switch (op) {
        case Opcode::MOV: return "MOV";
        case Opcode::MVN: return "MVN";
        case Opcode::MOVT: return "MOVT";
        case Opcode::MOVW: return "MOVW";
        case Opcode::ADD: return "ADD";
        case Opcode::ADC: return "ADC";
        case Opcode::SUB: return "SUB";
        case Opcode::SBC: return "SBC";
        case Opcode::RSB: return "RSB";
        case Opcode::MUL: return "MUL";
        case Opcode::MLA: return "MLA";
        case Opcode::AND: return "AND";
        case Opcode::ORR: return "ORR";
        case Opcode::EOR: return "EOR";
        case Opcode::BIC: return "BIC";
        case Opcode::CMP: return "CMP";
        case Opcode::CMN: return "CMN";
        case Opcode::TST: return "TST";
        case Opcode::TEQ: return "TEQ";
        case Opcode::ASR: return "ASR";
        case Opcode::LSL: return "LSL";
        case Opcode::LSR: return "LSR";
        case Opcode::ROR: return "ROR";
        case Opcode::B: return "B";
        case Opcode::BL: return "BL";
        case Opcode::BX: return "BX";
        case Opcode::BLX: return "BLX";
        case Opcode::CBZ: return "CBZ";
        case Opcode::CBNZ: return "CBNZ";
        case Opcode::LDR: return "LDR";
        case Opcode::LDRB: return "LDRB";
        case Opcode::LDRH: return "LDRH";
        case Opcode::LDRSB: return "LDRSB";
        case Opcode::LDRSH: return "LDRSH";
        case Opcode::STR: return "STR";
        case Opcode::STRB: return "STRB";
        case Opcode::STRH: return "STRH";
        case Opcode::LDM: return "LDM";
        case Opcode::STM: return "STM";
        case Opcode::PUSH: return "PUSH";
        case Opcode::POP: return "POP";
        case Opcode::SXTB: return "SXTB";
        case Opcode::SXTH: return "SXTH";
        case Opcode::UXTB: return "UXTB";
        case Opcode::UXTH: return "UXTH";
        case Opcode::REV: return "REV";
        case Opcode::REV16: return "REV16";
        case Opcode::REVSH: return "REVSH";
        case Opcode::UDIV: return "UDIV";
        case Opcode::SDIV: return "SDIV";
        case Opcode::ADR: return "ADR";
        case Opcode::CPSIE: return "CPSIE";
        case Opcode::CPSID: return "CPSID";
        case Opcode::WFI: return "WFI";
        case Opcode::DMB: return "DMB";
        case Opcode::DSB: return "DSB";
        case Opcode::ISB: return "ISB";
        case Opcode::MRS: return "MRS";
        case Opcode::MSR: return "MSR";
        case Opcode::SVC: return "SVC";
        case Opcode::NOP: return "NOP";
        case Opcode::UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

enum class ConditionCode : uint8_t {
    EQ = 0b0000, // Equal (Z == 1)
    NE = 0b0001, // Not equal (Z == 0)
    CS = 0b0010, // Carry set / Unsigned higher or same (C == 1)
    CC = 0b0011, // Carry clear / Unsigned lower (C == 0)
    MI = 0b0100, // Minus / Negative (N == 1)
    PL = 0b0101, // Plus / Positive or zero (N == 0)
    VS = 0b0110, // Overflow (V == 1)
    VC = 0b0111, // No overflow (V == 0)
    HI = 0b1000, // Unsigned higher (C == 1 and Z == 0)
    LS = 0b1001, // Unsigned lower or same (C == 0 or Z == 1)
    GE = 0b1010, // Signed greater than or equal (N == V)
    LT = 0b1011, // Signed less than (N != V)
    GT = 0b1100, // Signed greater than (Z == 0 and N == V)
    LE = 0b1101, // Signed less than or equal (Z == 1 or N != V)
    AL = 0b1110, // Always (unconditional)
    NV = 0b1111  // Never / Unpredictable
};

[[nodiscard]] constexpr bool condition_passed(ConditionCode cond, bool n, bool z, bool c, bool v) noexcept {
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
    return false;
}

enum class ShiftType : uint8_t {
    LSL = 0,
    LSR = 1,
    ASR = 2,
    ROR = 3,
    RRX = 4
};

enum class ImmediateCarryEffect : uint8_t {
    Preserve,
    Clear,
    Set
};

struct DecodedInstruction {
    Opcode op{Opcode::UNKNOWN};
    ConditionCode cond{ConditionCode::AL};
    uint8_t rd{0};
    uint8_t rn{0};
    uint8_t rm{0};
    uint8_t rs{0};

    bool is_imm{false};
    uint32_t imm{0};
    bool is_relative{false};

    ShiftType shift_type{ShiftType::LSL};
    uint8_t shift_amount{0};
    bool shift_by_reg{false};

    bool set_flags{false};
    ImmediateCarryEffect immediate_carry_effect{ImmediateCarryEffect::Preserve};
    uint8_t instr_size{2}; // 2 bytes for 16-bit Thumb, 4 bytes for 32-bit Thumb/ARM

    // Memory access parameters
    bool is_load{false};
    uint8_t mem_size{4}; // 1 = byte, 2 = halfword, 4 = word
    bool is_signed_mem{false};
    bool add_offset{true}; // true = base + offset, false = base - offset
    bool writeback{false};
    bool pre_indexed{true};
    uint16_t register_list{0}; // Bitmask for LDM/STM/PUSH/POP
    uint32_t raw_hex{0};       // Raw instruction encoding hex
    std::string disasm;        // Pre-computed disassembly text
};

} // namespace tinyarmsim
