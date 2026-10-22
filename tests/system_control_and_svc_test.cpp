#include <gtest/gtest.h>
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/instruction.hpp"
#include "tinyarmsim/decoder.hpp"
#include "tinyarmsim/interpreter.hpp"

using namespace tinyarmsim;

class SystemControlAndSvcTest : public ::testing::Test {
protected:
    ArchitecturalState state;
    MemoryBus bus{1024 * 1024};
    IsaInterpreter interpreter{state, bus};
};

TEST_F(SystemControlAndSvcTest, MrsAndMsrManipulateFlags) {
    // Set NZCV flags: N=1, Z=0, C=1, V=0 -> CPSR flags = 0xA0000000
    state.set_flags(true, false, true, false);

    // MRS R0, APSR (read CPSR/APSR into R0)
    DecodedInstruction mrs_op{};
    mrs_op.op = Opcode::MRS;
    mrs_op.rd = 0;
    mrs_op.instr_size = 4;

    interpreter.execute(mrs_op);
    EXPECT_EQ(state.get_reg(0) & 0xF0000000u, 0xA0000000u);

    // Now modify R1 to 0x40000000 (Z=1, others 0) and write back via MSR
    state.set_reg(1, 0x40000000u);
    DecodedInstruction msr_op{};
    msr_op.op = Opcode::MSR;
    msr_op.rn = 1;
    msr_op.instr_size = 4;

    interpreter.execute(msr_op);
    EXPECT_FALSE(state.get_flag_n());
    EXPECT_TRUE(state.get_flag_z());
    EXPECT_FALSE(state.get_flag_c());
    EXPECT_FALSE(state.get_flag_v());
}

TEST_F(SystemControlAndSvcTest, NopAdvancesPc) {
    state.set_pc(0x1000);
    DecodedInstruction nop_op{};
    nop_op.op = Opcode::NOP;
    nop_op.instr_size = 2;

    interpreter.execute(nop_op);
    EXPECT_EQ(state.get_pc(), 0x1002u);
}

TEST_F(SystemControlAndSvcTest, RunLoopHaltsOnSvcAndReturnsR0) {
    // Write a small program into memory:
    // 0x1000: MOVS R0, #42   (0x202A)
    // 0x1002: SVC #0         (0xDF00)
    bus.write16(0x1000, 0x202A);
    bus.write16(0x1002, 0xDF00);

    state.set_pc(0x1000);
    uint32_t exit_code = interpreter.run(100);

    EXPECT_EQ(exit_code, 42u);
    EXPECT_EQ(state.get_reg(0), 42u);
    EXPECT_EQ(state.get_pc(), 0x1002u);
}

TEST_F(SystemControlAndSvcTest, RunLoopDetectsMaxStepTimeout) {
    // Infinite loop: 0x1000: B 0x1000 (0xE7FE: imm11 = -1 -> offset = -2 -> PC+4-2 = 0x1002?? offset=-1*2=-2 -> 0x1000+4-4 = 0x1000)
    bus.write16(0x1000, 0xE7FE);
    state.set_pc(0x1000);

    EXPECT_THROW(interpreter.run(50), CpuFaultException);
}

// === Decoder Tests for MRS / MSR ===

TEST(DecoderSystemTest, DecodeThumb32MrsMsr) {
    // MRS R0, APSR: 0xF3EF 0x8000
    uint16_t w1_mrs = 0xF3EF;
    uint16_t w2_mrs = 0x8000;
    DecodedInstruction mrs_instr = Decoder::decode32(w1_mrs, w2_mrs);

    EXPECT_EQ(mrs_instr.op, Opcode::MRS);
    EXPECT_EQ(mrs_instr.rd, 0u);
    EXPECT_EQ(mrs_instr.instr_size, 4u);

    // MSR APSR_nzcvq, R1: 0xF381 0x8800
    uint16_t w1_msr = 0xF381;
    uint16_t w2_msr = 0x8800;
    DecodedInstruction msr_instr = Decoder::decode32(w1_msr, w2_msr);

    EXPECT_EQ(msr_instr.op, Opcode::MSR);
    EXPECT_EQ(msr_instr.rn, 1u);
    EXPECT_EQ(msr_instr.instr_size, 4u);
}

TEST(SystemControlTest, Thumb1BareMetalExtensionsAndPrimask) {
    ArchitecturalState state;
    MemoryBus bus{1024 * 1024};
    IsaInterpreter interp(state, bus);

    EXPECT_FALSE(state.get_primask());

    // Test CPSID i (0xB672) & CPSIE i (0xB662)
    bus.write16(0x1000, 0xB672); // CPSID i
    bus.write16(0x1002, 0xB662); // CPSIE i
    bus.write16(0x1004, 0xBF30); // WFI
    state.set_pc(0x1000);

    interp.step();
    EXPECT_TRUE(state.get_primask());
    interp.step();
    EXPECT_FALSE(state.get_primask());
    interp.step(); // WFI (NOP behavior)
    EXPECT_EQ(state.get_pc(), 0x1006u);

    // Test Adjust SP: SUB SP, #16 (0xB084) and ADD SP, #16 (0xB004)
    state.set_sp(0x20000000);
    bus.write16(0x1006, 0xB084); // SUB SP, SP, #16
    bus.write16(0x1008, 0xB004); // ADD SP, SP, #16
    interp.step();
    EXPECT_EQ(state.get_sp(), 0x20000000u - 16u);
    interp.step();
    EXPECT_EQ(state.get_sp(), 0x20000000u);

    // Test ADD Rd, SP, #imm8: ADD R2, SP, #8 (0xA800 | (2 << 8) | 2 = 0xAA02)
    bus.write16(0x100A, 0xAA02);
    interp.step();
    EXPECT_EQ(state.get_reg(2), 0x20000008u);

    // Test ADR Rd, #imm8: ADR R1, #12 (0xA000 | (1 << 8) | 3 = 0xA103)
    // At PC=0x100C: aligned_pc = (0x100C + 4) & ~3 = 0x1010. Imm = 3 * 4 = 12. Result = 0x101C.
    bus.write16(0x100C, 0xA103);
    interp.step();
    EXPECT_EQ(state.get_reg(1), 0x101Cu);

    // Test SXTB, SXTH, UXTB, UXTH
    // R0 = 0x00008080
    state.set_reg(0, 0x00008080);
    // SXTB R3, R0: 0xB240 | (0 << 3) | 3 = 0xB243
    // SXTH R4, R0: 0xB200 | (0 << 3) | 4 = 0xB204
    // UXTB R5, R0: 0xB2C0 | (0 << 3) | 5 = 0xB2C5
    // UXTH R6, R0: 0xB280 | (0 << 3) | 6 = 0xB286
    bus.write16(0x100E, 0xB243);
    bus.write16(0x1010, 0xB204);
    bus.write16(0x1012, 0xB2C5);
    bus.write16(0x1014, 0xB286);
    interp.step();
    EXPECT_EQ(state.get_reg(3), 0xFFFFFF80u); // SXTB 0x80 -> -128
    interp.step();
    EXPECT_EQ(state.get_reg(4), 0xFFFF8080u); // SXTH 0x8080 -> -32640
    interp.step();
    EXPECT_EQ(state.get_reg(5), 0x80u);       // UXTB
    interp.step();
    EXPECT_EQ(state.get_reg(6), 0x8080u);     // UXTH

    // Test REV, REV16, REVSH
    // R0 = 0x12345680
    // REV -> 0x80563412
    // REV16 -> 0x34128056
    // REVSH: lower halfword is 0x5680, byte swapped -> 0x8056 (bit 15 is 1, so negative), sign-extended -> 0xFFFF8056
    state.set_reg(0, 0x12345680);
    // REV R3, R0: 0xBA00 | (0 << 3) | 3 = 0xBA03
    // REV16 R4, R0: 0xBA40 | (0 << 3) | 4 = 0xBA44
    // REVSH R5, R0: 0xBAC0 | (0 << 3) | 5 = 0xBAC5
    bus.write16(0x1016, 0xBA03);
    bus.write16(0x1018, 0xBA44);
    bus.write16(0x101A, 0xBAC5);
    interp.step();
    EXPECT_EQ(state.get_reg(3), 0x80563412u);
    interp.step();
    EXPECT_EQ(state.get_reg(4), 0x34128056u);
    interp.step();
    EXPECT_EQ(state.get_reg(5), 0xFFFF8056u);

}
