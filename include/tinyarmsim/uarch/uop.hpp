#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include "tinyarmsim/common.hpp"
#include "tinyarmsim/instruction.hpp"
#include "tinyarmsim/uarch/branch_predictor.hpp"
#include "tinyarmsim/uarch/rat_prf.hpp"

namespace tinyarmsim::uarch {

enum class UOpType : uint8_t {
    NOP,
    ALU,            // Integer arithmetic / logic
    MUL,            // Multiply / MAC
    DIV,            // Divide
    BRANCH,         // Conditional / unconditional branch
    CALL,           // Function call (BL / BLX)
    RET,            // Return (BX LR / POP PC)
    LOAD,           // Memory Load (LDR, LDRB, LDRH, LDRSB, LDRSH)
    STORE_ADDR,     // Memory Store Address Generation (STA)
    STORE_DATA,     // Memory Store Data (STD)
    ATOMIC_LOAD,    // LDREX
    ATOMIC_STORE,   // STREX
    SYS_REG,        // MRS / MSR / CPS
    FENCE,          // DMB / DSB / ISB
    SVC,            // Supervisor call / Exception
    HALT
};

enum class ExecutionPort : uint8_t {
    PORT_NONE,
    PORT_0_ALU_BRANCH,  // ALU 0 + Branch Unit
    PORT_1_ALU_MUL,     // ALU 1 + Multiplier / Divider
    PORT_2_LOAD_AGU,    // Load AGU 0 + L1D Read Port 0
    PORT_3_DUAL_AGU,    // Dual Load AGU 1 / Store AGU (STA) + L1D Read Port 1
    PORT_4_STORE_DATA   // Store Data Port (STD)
};

struct UOp {
    uint64_t uop_id{0};             // Global unique sequence ID
    uint64_t seq_num{0};            // Instruction sequence number
    uint32_t pc{0};                 // Instruction PC address
    uint32_t raw_inst{0};           // Raw instruction encoding
    bool is_thumb32{false};         // True if 32-bit Thumb-2 instruction
    bool is_last_uop_of_macro_inst{true}; // True if this uop is the last uop retiring the macro-instruction

    UOpType type{UOpType::NOP};
    ExecutionPort target_port{ExecutionPort::PORT_0_ALU_BRANCH};
    Opcode opcode{Opcode::NOP};     // Functional ISA opcode
    ConditionCode cond{ConditionCode::AL}; // Condition code for conditional execution/branches

    // Architectural registers (0..15, 0xFF = invalid/none, 16 = APSR / CPSR flags)
    static constexpr uint8_t INVALID_REG = 0xFF;
    static constexpr uint8_t ARCH_REG_FLAGS = 16;
    uint8_t arch_src1{INVALID_REG};
    uint8_t arch_src2{INVALID_REG};
    uint8_t arch_src3{INVALID_REG}; // E.g. for MLA / Store data
    uint8_t arch_dest{INVALID_REG};

    // Renamed physical registers
    uint16_t phys_src1{0};
    uint16_t phys_src2{0};
    uint16_t phys_src3{0};
    uint16_t phys_dest{0};
    uint16_t old_phys_dest{0};      // Replaced physical register (for ROB retirement)

    // Flag renaming
    bool sets_flags{false};
    uint16_t phys_flags_dest{0};
    uint16_t old_phys_flags_dest{0};
    uint16_t phys_flags_src{0};
    bool flags_src_ready{true};
    uint32_t flags_val{0};

    // Ready flags for issue queue wakeup
    bool src1_ready{true};
    bool src2_ready{true};
    bool src3_ready{true};

    // Immediate & displacement operands
    uint32_t imm{0};
    int32_t offset{0};
    bool is_imm_valid{false};

    // Branch prediction metadata
    bool is_branch{false};
    bool pred_taken{false};
    uint32_t pred_target{0};
    bool actual_taken{false};
    uint32_t actual_target{0};
    bool branch_mispredicted{false};
    BranchPrediction branch_pred{};
    RegisterAliasTable::Checkpoint rat_checkpoint{};

    // Memory subsystem metadata
    uint32_t mem_addr{0};
    uint32_t mem_data{0};
    uint8_t mem_size_bytes{4};      // 1, 2, 4
    bool is_signed_mem{false};
    size_t rob_idx{0};              // Reorder buffer entry index
    size_t lsu_queue_idx{0};        // Load/Store queue entry index
    bool mem_forwarded{false};      // Hit in store-to-load forwarding

    // Macro-Op Fusion metadata
    bool is_fused{false};
    Opcode fused_cmp_opcode{Opcode::UNKNOWN};

    // Pipeline tracking states
    uint64_t ready_cycle{0};
    bool executed{false};
    bool ready_to_commit{false};
    bool is_squashed{false};

    [[nodiscard]] std::string to_string() const {
        std::ostringstream oss;
        oss << "uop#" << uop_id << " [0x" << std::hex << pc << std::dec << "] ";
        switch (type) {
            case UOpType::ALU: oss << "ALU"; break;
            case UOpType::MUL: oss << "MUL"; break;
            case UOpType::DIV: oss << "DIV"; break;
            case UOpType::BRANCH: oss << "BRANCH"; break;
            case UOpType::CALL: oss << "CALL"; break;
            case UOpType::RET: oss << "RET"; break;
            case UOpType::LOAD: oss << "LOAD"; break;
            case UOpType::STORE_ADDR: oss << "STA"; break;
            case UOpType::STORE_DATA: oss << "STD"; break;
            case UOpType::ATOMIC_LOAD: oss << "LDREX"; break;
            case UOpType::ATOMIC_STORE: oss << "STREX"; break;
            case UOpType::SYS_REG: oss << "SYS"; break;
            case UOpType::SVC: oss << "SVC"; break;
            case UOpType::NOP: oss << "NOP"; break;
            default: oss << "OTHER"; break;
        }
        if (arch_dest != INVALID_REG) {
            oss << " -> r" << static_cast<int>(arch_dest) << "(p" << phys_dest << ")";
        }
        return oss.str();
    }
};

} // namespace tinyarmsim::uarch
