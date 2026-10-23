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
    state.set_flags(true, false, true, true);
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
    EXPECT_TRUE(state.get_flag_n());
    EXPECT_FALSE(state.get_flag_z());
    EXPECT_TRUE(state.get_flag_c());
    EXPECT_TRUE(state.get_flag_v());

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
    EXPECT_TRUE(state.get_flag_n());
    EXPECT_FALSE(state.get_flag_z());
    EXPECT_TRUE(state.get_flag_c());
    EXPECT_TRUE(state.get_flag_v());

}

TEST(SystemControlTest, Thumb2DivideBranchPushPopAndIndexedLdrStr) {
    ArchitecturalState state;
    MemoryBus bus{1024 * 1024};
    IsaInterpreter interp(state, bus);

    // 1. Test UDIV and SDIV (including div by zero)
    // UDIV R2, R0, R1: w1 = 0xFBB0, w2 = 0xF2F1
    // SDIV R3, R0, R1: w1 = 0xFB90, w2 = 0xF3F1
    state.set_reg(0, 100u);
    state.set_reg(1, 20u);
    bus.write16(0x2000, 0xFBB0); bus.write16(0x2002, 0xF2F1); // UDIV R2, R0, R1
    bus.write16(0x2004, 0xFB90); bus.write16(0x2006, 0xF3F1); // SDIV R3, R0, R1
    state.set_pc(0x2000);
    interp.step();
    EXPECT_EQ(state.get_reg(2), 5u);
    interp.step();
    EXPECT_EQ(state.get_reg(3), 5u);

    // Signed negative division and division by zero
    state.set_reg(0, static_cast<uint32_t>(-100));
    state.set_reg(1, 20u);
    bus.write16(0x2008, 0xFB90); bus.write16(0x200A, 0xF3F1); // SDIV R3, R0, R1 -> -5
    state.set_reg(4, 0u);
    bus.write16(0x200C, 0xFBB0); bus.write16(0x200E, 0xF5F4); // UDIV R5, R0, R4 -> 0
    bus.write16(0x2010, 0xFB90); bus.write16(0x2012, 0xF6F4); // SDIV R6, R0, R4 -> 0
    interp.step();
    EXPECT_EQ(static_cast<int32_t>(state.get_reg(3)), -5);
    interp.step();
    EXPECT_EQ(state.get_reg(5), 0u);
    interp.step();
    EXPECT_EQ(state.get_reg(6), 0u);

    // 2. Test PUSH.W and POP.W with high registers (r8, r9, lr)
    // PUSH.W {r8, r9, lr}: w1 = 0xE92D, w2 = (1<<8)|(1<<9)|(1<<14) = 0x4300
    // POP.W  {r8, r9, pc}: w1 = 0xE8BD, w2 = (1<<8)|(1<<9)|(1<<15) = 0x8300
    state.set_sp(0x20000);
    state.set_reg(8, 0x11112222u);
    state.set_reg(9, 0x33334444u);
    state.set_lr(0x2021u); // return to 0x2020 with thumb bit
    bus.write16(0x2014, 0xE92D); bus.write16(0x2016, 0x4300);
    bus.write16(0x2018, 0xE8BD); bus.write16(0x201A, 0x8300);
    interp.step(); // PUSH.W
    EXPECT_EQ(state.get_sp(), 0x20000u - 12u);
    state.set_reg(8, 0u);
    state.set_reg(9, 0u);
    interp.step(); // POP.W {r8, r9, pc}
    EXPECT_EQ(state.get_sp(), 0x20000u);
    EXPECT_EQ(state.get_reg(8), 0x11112222u);
    EXPECT_EQ(state.get_reg(9), 0x33334444u);
    EXPECT_EQ(state.get_pc(), 0x2020u);

    // 3. Test Extended LDR.W / STR.W (post-indexed, negative offset, shifted register)
    state.set_reg(0, 0x30000u);
    state.set_reg(1, 0xCAFEBABEu);
    // STR.W R1, [R0], #4 (post-indexed: P=0, U=1, W=1, imm8=4 -> w2 = 0x1B04)
    // w1 = 0xF840 (size=10, L=0, rn=0)
    bus.write16(0x2020, 0xF840); bus.write16(0x2022, 0x1B04);
    // LDR.W R2, [R0, #-4] (negative offset: P=1, U=0, W=0, imm8=4 -> w2 = 0x2C04)
    // w1 = 0xF850 (size=10, L=1, rn=0)
    bus.write16(0x2024, 0xF850); bus.write16(0x2026, 0x2C04);
    // LDR.W R3, [R0, R4, LSL #2] (shifted register: w1 = 0xF850, w2 = (3<<12) | (2<<4) | 4 = 0x3024)
    state.set_reg(4, 2u);
    bus.write32(0x30004u + 8u, 0xDEADBEEFu);
    bus.write16(0x2028, 0xF850); bus.write16(0x202A, 0x3024);

    interp.step(); // STR.W R1, [R0], #4
    EXPECT_EQ(state.get_reg(0), 0x30004u);
    EXPECT_EQ(bus.read32(0x30000u), 0xCAFEBABEu);
    interp.step(); // LDR.W R2, [R0, #-4]
    EXPECT_EQ(state.get_reg(2), 0xCAFEBABEu);
    interp.step(); // LDR.W R3, [R0, R4, LSL #2]
    EXPECT_EQ(state.get_reg(3), 0xDEADBEEFu);

    // 4. Test DMB (0xF3BF 0x8F5F), DSB (0xF3BF 0x8F4F), ISB (0xF3BF 0x8F6F)
    bus.write16(0x202C, 0xF3BF); bus.write16(0x202E, 0x8F5F);
    bus.write16(0x2030, 0xF3BF); bus.write16(0x2032, 0x8F4F);
    bus.write16(0x2034, 0xF3BF); bus.write16(0x2036, 0x8F6F);
    state.set_pc(0x202C);
    state.set_flags(true, false, true, true);
    interp.step();
    EXPECT_EQ(state.get_pc(), 0x2030u);
    interp.step();
    EXPECT_EQ(state.get_pc(), 0x2034u);
    interp.step();
    EXPECT_EQ(state.get_pc(), 0x2038u);
    EXPECT_TRUE(state.get_flag_n());
    EXPECT_FALSE(state.get_flag_z());
    EXPECT_TRUE(state.get_flag_c());
    EXPECT_TRUE(state.get_flag_v());

    // 5. Test B.W and B<cond>.W
    // At PC=0x2038: B.W +8 (target = 0x2038 + 4 + 8 = 0x2044)
    // s=0, j1=1, j2=1 (so i1=1, i2=1? No, for s=0, i1 = ~(j1^0)&1 = ~j1&1, so j1=1 -> i1=0, j2=1 -> i2=0!)
    // Thus for positive small offset: s=0, j1=1, j2=1 -> i1=0, i2=0, imm10=0, imm11=4 (4<<1 = 8):
    // w1 = 0xF000, w2 = 0x9000 | (1<<13) | (1<<11) | 4 = 0xB804
    bus.write16(0x2038, 0xF000); bus.write16(0x203A, 0xB804);
    // At PC=0x2044: BEQ.W +8 (target = 0x2044 + 4 + 8 = 0x2050)
    // cond=0 (EQ), s=0, j1=0, j2=0, imm6=0, imm11=4 -> w1 = 0xF000, w2 = 0x8004
    bus.write16(0x2044, 0xF000); bus.write16(0x2046, 0x8004);

    interp.step(); // B.W -> 0x2044
    EXPECT_EQ(state.get_pc(), 0x2044u);

    state.set_flags(false, true, false, false); // Z = 1 (EQ true)
    interp.step(); // BEQ.W -> 0x2050
    EXPECT_EQ(state.get_pc(), 0x2050u);
}
