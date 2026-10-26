#include <gtest/gtest.h>
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/instruction.hpp"
#include "tinyarmsim/decoder.hpp"
#include "tinyarmsim/interpreter.hpp"

using namespace tinyarmsim;

class BranchingTest : public ::testing::Test {
protected:
    ArchitecturalState state;
    MemoryBus bus{1024 * 1024};
    IsaInterpreter interpreter{state, bus};
};

TEST_F(BranchingTest, UnconditionalBranchB) {
    state.set_pc(0x1000);

    // B #+8 (jump forward 8 bytes -> PC + 4 + 8 in ARM pipeline convention, or PC + offset)
    // In our decoder/interpreter: target PC = current_pc + 4 + (offset)
    DecodedInstruction b_op{};
    b_op.op = Opcode::B;
    b_op.is_imm = true;
    b_op.imm = 0x1008; // Target address directly or resolved offset
    b_op.instr_size = 2;

    interpreter.execute(b_op);
    EXPECT_EQ(state.get_pc(), 0x1008u);
}

TEST_F(BranchingTest, ConditionalBranchTakenAndNotTaken) {
    // 1. BEQ when Z=1 (Taken)
    state.set_pc(0x2000);
    state.set_flag_z(true);

    DecodedInstruction beq_taken{};
    beq_taken.op = Opcode::B;
    beq_taken.cond = ConditionCode::EQ;
    beq_taken.is_imm = true;
    beq_taken.imm = 0x2050;
    beq_taken.instr_size = 2;

    interpreter.execute(beq_taken);
    EXPECT_EQ(state.get_pc(), 0x2050u);

    // 2. BEQ when Z=0 (Not Taken -> advances PC by 2)
    state.set_flag_z(false);
    DecodedInstruction beq_nottaken = beq_taken;
    beq_nottaken.imm = 0x2100;

    interpreter.execute(beq_nottaken);
    EXPECT_EQ(state.get_pc(), 0x2052u); // 0x2050 + 2
}

TEST_F(BranchingTest, FunctionCallAndReturnViaBlAndBx) {
    state.set_pc(0x1000);

    // BL to 0x3000
    DecodedInstruction bl_op{};
    bl_op.op = Opcode::BL;
    bl_op.is_imm = true;
    bl_op.imm = 0x3000;
    bl_op.instr_size = 4;

    interpreter.execute(bl_op);
    // In ARM Thumb mode, LR = (return address) | 1
    // Return address after 4-byte instruction at 0x1000 is 0x1004
    EXPECT_EQ(state.get_pc(), 0x3000u);
    EXPECT_EQ(state.get_lr(), 0x1005u); // 0x1004 | 1

    // Inside function: BX LR (return)
    DecodedInstruction bx_lr{};
    bx_lr.op = Opcode::BX;
    bx_lr.rm = 14; // LR
    bx_lr.instr_size = 2;

    interpreter.execute(bx_lr);
    // PC should jump back to 0x1004 (clearing bit 0 for alignment)
    EXPECT_EQ(state.get_pc(), 0x1004u);
}

TEST_F(BranchingTest, CbzAndCbnz) {
    state.set_pc(0x4000);
    state.set_reg(0, 0); // R0 is 0

    // CBZ R0, target (0x4020) -> Taken because R0 is 0
    DecodedInstruction cbz_taken{};
    cbz_taken.op = Opcode::CBZ;
    cbz_taken.rn = 0;
    cbz_taken.is_imm = true;
    cbz_taken.imm = 0x4020;
    cbz_taken.instr_size = 2;

    interpreter.execute(cbz_taken);
    EXPECT_EQ(state.get_pc(), 0x4020u);

    // CBNZ R0, target (0x4050) -> Not taken because R0 is 0
    DecodedInstruction cbnz_nottaken{};
    cbnz_nottaken.op = Opcode::CBNZ;
    cbnz_nottaken.rn = 0;
    cbnz_nottaken.is_imm = true;
    cbnz_nottaken.imm = 0x4050;
    cbnz_nottaken.instr_size = 2;

    interpreter.execute(cbnz_nottaken);
    EXPECT_EQ(state.get_pc(), 0x4022u); // 0x4020 + 2
}

// === Decoder Tests for Branches ===

TEST(DecoderBranchTest, DecodeThumb16UnconditionalBranchB) {
    // 0xE005 = B #+10 (offset = 5 * 2 = 10 bytes)
    // From PC=0x1000: target = 0x1000 + 4 + 10 = 0x100E
    uint16_t raw_b = 0xE005;
    DecodedInstruction instr = Decoder::decode16(raw_b, 0x1000);

    EXPECT_EQ(instr.op, Opcode::B);
    EXPECT_EQ(instr.cond, ConditionCode::AL);
    EXPECT_TRUE(instr.is_imm);
    EXPECT_EQ(instr.imm, 0x100Eu);
}

TEST(DecoderBranchTest, DecodeThumb16ConditionalBranchBeq) {
    // 0xD004 = BEQ #+8 (offset = 4 * 2 = 8 bytes)
    // From PC=0x2000: target = 0x2000 + 4 + 8 = 0x200C
    uint16_t raw_beq = 0xD004;
    DecodedInstruction instr = Decoder::decode16(raw_beq, 0x2000);

    EXPECT_EQ(instr.op, Opcode::B);
    EXPECT_EQ(instr.cond, ConditionCode::EQ);
    EXPECT_TRUE(instr.is_imm);
    EXPECT_EQ(instr.imm, 0x200Cu);
}

TEST(DecoderBranchTest, DecodeThumb16Bx) {
    // 0x4770 = BX LR (Rm = 14)
    uint16_t raw_bx = 0x4770;
    DecodedInstruction instr = Decoder::decode16(raw_bx, 0x3000);

    EXPECT_EQ(instr.op, Opcode::BX);
    EXPECT_EQ(instr.rm, 14u);
}

TEST(DecoderBranchTest, DecodeThumb16Cbz) {
    // 0xB120 = CBZ R0, #8 (i=0, imm5=00100 -> offset = 4*2 = 8)
    // From PC=0x4000: target = 0x4000 + 4 + 8 = 0x400C
    uint16_t raw_cbz = 0xB120;
    DecodedInstruction instr = Decoder::decode16(raw_cbz, 0x4000);

    EXPECT_EQ(instr.op, Opcode::CBZ);
    EXPECT_EQ(instr.rn, 0u);
    EXPECT_TRUE(instr.is_imm);
    EXPECT_EQ(instr.imm, 0x400Cu);
}

TEST(DecoderBranchTest, DecodeThumb32Bl) {
    // 32-bit BL: w1 = 0xF000, w2 = 0xF805 (offset = +10)
    // From PC=0x1000: target = 0x1000 + 4 + 10 = 0x100E
    uint16_t w1 = 0xF000;
    uint16_t w2 = 0xF805;
    DecodedInstruction instr = Decoder::decode32(w1, w2, 0x1000);

    EXPECT_EQ(instr.op, Opcode::BL);
    EXPECT_TRUE(instr.is_imm);
    EXPECT_EQ(instr.imm, 0x100Eu);
    EXPECT_EQ(instr.instr_size, 4u);
}


TEST(BranchConditionCharacterization, PublicBranchMatchesLiteralConditionMasks) {
    ArchitecturalState state;
    MemoryBus bus(1024);
    IsaInterpreter interpreter(state, bus);
    constexpr ConditionCode conditions[] = {
        ConditionCode::EQ, ConditionCode::NE, ConditionCode::CS, ConditionCode::CC,
        ConditionCode::MI, ConditionCode::PL, ConditionCode::VS, ConditionCode::VC,
        ConditionCode::HI, ConditionCode::LS, ConditionCode::GE, ConditionCode::LT,
        ConditionCode::GT, ConditionCode::LE, ConditionCode::AL, ConditionCode::NV};
    constexpr uint16_t truth_masks[] = {
        0xF0F0, 0x0F0F, 0xCCCC, 0x3333, 0xFF00, 0x00FF, 0xAAAA, 0x5555,
        0x0C0C, 0xF3F3, 0xAA55, 0x55AA, 0x0A05, 0xF5FA, 0xFFFF, 0x0000};
    DecodedInstruction branch{};
    branch.op = Opcode::B;
    branch.is_imm = true;
    branch.imm = 0x8000;
    branch.instr_size = 2;

    for (size_t cond_index = 0; cond_index < 16; ++cond_index) {
        branch.cond = conditions[cond_index];
        for (unsigned flags = 0; flags < 16; ++flags) {
            const bool n = (flags & 0x8) != 0;
            const bool z = (flags & 0x4) != 0;
            const bool c = (flags & 0x2) != 0;
            const bool v = (flags & 0x1) != 0;
            state.set_flag_n(n);
            state.set_flag_z(z);
            state.set_flag_c(c);
            state.set_flag_v(v);
            state.set_pc(0x1000);
            interpreter.execute(branch);
            const bool expected_taken = ((truth_masks[cond_index] >> flags) & 1u) != 0;
            EXPECT_EQ(state.get_pc(), expected_taken ? 0x8000u : 0x1002u)
                << "condition=" << cond_index << " NZCV=" << flags;
        }
    }

    EXPECT_FALSE(interpreter.evaluate_condition(static_cast<ConditionCode>(0xFF)));
}
