#include <gtest/gtest.h>
#include "tinyarmsim/cpu/state.hpp"
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/cpu/instruction.hpp"
#include "tinyarmsim/cpu/decoder.hpp"
#include "tinyarmsim/cpu/interpreter.hpp"

using namespace tinyarmsim;

class BasicDataProcessingTest : public ::testing::Test {
protected:
    ArchitecturalState state;
    MemoryBus bus{1024 * 1024}; // 1MB for unit tests
    IsaInterpreter interpreter{state, bus};
};

TEST_F(BasicDataProcessingTest, MovImmediateAndRegister) {
    // MOV R0, #42
    DecodedInstruction mov_imm{};
    mov_imm.op = Opcode::MOV;
    mov_imm.rd = 0;
    mov_imm.is_imm = true;
    mov_imm.imm = 42;
    mov_imm.instr_size = 2;

    interpreter.execute(mov_imm);
    EXPECT_EQ(state.get_reg(0), 42u);
    EXPECT_EQ(state.get_pc(), 2u);

    // MOV R1, R0
    DecodedInstruction mov_reg{};
    mov_reg.op = Opcode::MOV;
    mov_reg.rd = 1;
    mov_reg.rm = 0;
    mov_reg.is_imm = false;
    mov_reg.instr_size = 2;

    interpreter.execute(mov_reg);
    EXPECT_EQ(state.get_reg(1), 42u);
    EXPECT_EQ(state.get_pc(), 4u);
}

TEST_F(BasicDataProcessingTest, MvnImmediate) {
    // MVN R0, #0
    DecodedInstruction mvn{};
    mvn.op = Opcode::MVN;
    mvn.rd = 0;
    mvn.is_imm = true;
    mvn.imm = 0;
    mvn.instr_size = 2;

    interpreter.execute(mvn);
    EXPECT_EQ(state.get_reg(0), 0xFFFFFFFFu);
}

TEST_F(BasicDataProcessingTest, AddWithoutFlags) {
    state.set_reg(1, 100);
    state.set_reg(2, 250);

    // ADD R0, R1, R2
    DecodedInstruction add_op{};
    add_op.op = Opcode::ADD;
    add_op.rd = 0;
    add_op.rn = 1;
    add_op.rm = 2;
    add_op.set_flags = false;
    add_op.instr_size = 2;

    interpreter.execute(add_op);
    EXPECT_EQ(state.get_reg(0), 350u);
    EXPECT_FALSE(state.get_flag_z());
    EXPECT_FALSE(state.get_flag_c());
}

TEST_F(BasicDataProcessingTest, AddsFlagCalculations) {
    // 1. Zero flag test: 5 + (-5) = 0
    state.set_reg(1, 5);
    state.set_reg(2, static_cast<uint32_t>(-5));

    DecodedInstruction adds1{};
    adds1.op = Opcode::ADD;
    adds1.rd = 0;
    adds1.rn = 1;
    adds1.rm = 2;
    adds1.set_flags = true;
    adds1.instr_size = 2;

    interpreter.execute(adds1);
    EXPECT_EQ(state.get_reg(0), 0u);
    EXPECT_TRUE(state.get_flag_z());
    EXPECT_FALSE(state.get_flag_n());
    EXPECT_TRUE(state.get_flag_c()); // Carry generated in unsigned addition
    EXPECT_FALSE(state.get_flag_v());

    // 2. Signed Overflow test: 0x7FFFFFFF + 1 = 0x80000000 (Positive + Positive = Negative)
    state.set_reg(1, 0x7FFFFFFF);
    state.set_reg(2, 1);

    DecodedInstruction adds2{};
    adds2.op = Opcode::ADD;
    adds2.rd = 0;
    adds2.rn = 1;
    adds2.rm = 2;
    adds2.set_flags = true;
    adds2.instr_size = 2;

    interpreter.execute(adds2);
    EXPECT_EQ(state.get_reg(0), 0x80000000u);
    EXPECT_TRUE(state.get_flag_n());
    EXPECT_FALSE(state.get_flag_z());
    EXPECT_FALSE(state.get_flag_c());
    EXPECT_TRUE(state.get_flag_v()); // Signed overflow!
}

TEST_F(BasicDataProcessingTest, SubAndSubs) {
    state.set_reg(1, 10);
    state.set_reg(2, 20);

    // SUBS R0, R1, R2 -> 10 - 20 = -10 (0xFFFFFFF6)
    DecodedInstruction subs{};
    subs.op = Opcode::SUB;
    subs.rd = 0;
    subs.rn = 1;
    subs.rm = 2;
    subs.set_flags = true;
    subs.instr_size = 2;

    interpreter.execute(subs);
    EXPECT_EQ(state.get_reg(0), 0xFFFFFFF6u);
    EXPECT_TRUE(state.get_flag_n());
    EXPECT_FALSE(state.get_flag_z());
    EXPECT_FALSE(state.get_flag_c()); // Borrow occurred (Carry clear on borrow in ARM)
    EXPECT_FALSE(state.get_flag_v());
}

TEST_F(BasicDataProcessingTest, CmpUpdatesFlagsWithoutModifyingRegisters) {
    state.set_reg(0, 100);
    state.set_reg(1, 100);

    // CMP R0, R1
    DecodedInstruction cmp{};
    cmp.op = Opcode::CMP;
    cmp.rn = 0;
    cmp.rm = 1;
    cmp.set_flags = true;
    cmp.instr_size = 2;

    interpreter.execute(cmp);
    EXPECT_EQ(state.get_reg(0), 100u); // Unchanged
    EXPECT_EQ(state.get_reg(1), 100u); // Unchanged
    EXPECT_TRUE(state.get_flag_z());
    EXPECT_TRUE(state.get_flag_c());   // No borrow -> Carry set
    EXPECT_FALSE(state.get_flag_n());
}

// === Decoder Tests for 16-bit Thumb Encodings ===

TEST(DecoderTest, DecodeThumb16MovImmediate) {
    // 0x202A = MOVS R0, #42 (Thumb 16-bit: 00100 Rd(000) Imm(00101010))
    uint16_t raw_mov = 0x202A;
    DecodedInstruction instr = Decoder::decode16(raw_mov);

    EXPECT_EQ(instr.op, Opcode::MOV);
    EXPECT_EQ(instr.rd, 0u);
    EXPECT_TRUE(instr.is_imm);
    EXPECT_EQ(instr.imm, 42u);
    EXPECT_TRUE(instr.set_flags);
    EXPECT_EQ(instr.instr_size, 2u);
}

TEST(DecoderTest, DecodeThumb16AddRegister) {
    // 0x1888 = ADDS R0, R1, R2 (Thumb 16-bit: 0001100 Rm(010) Rn(001) Rd(000))
    uint16_t raw_add = 0x1888;
    DecodedInstruction instr = Decoder::decode16(raw_add);

    EXPECT_EQ(instr.op, Opcode::ADD);
    EXPECT_EQ(instr.rd, 0u);
    EXPECT_EQ(instr.rn, 1u);
    EXPECT_EQ(instr.rm, 2u);
    EXPECT_FALSE(instr.is_imm);
    EXPECT_TRUE(instr.set_flags);
    EXPECT_EQ(instr.instr_size, 2u);
}

TEST(DecoderTest, DecodeThumb16SubRegister) {
    // 0x1A88 = SUBS R0, R1, R2 (Thumb 16-bit: 0001101 Rm(010) Rn(001) Rd(000))
    uint16_t raw_sub = 0x1A88;
    DecodedInstruction instr = Decoder::decode16(raw_sub);

    EXPECT_EQ(instr.op, Opcode::SUB);
    EXPECT_EQ(instr.rd, 0u);
    EXPECT_EQ(instr.rn, 1u);
    EXPECT_EQ(instr.rm, 2u);
    EXPECT_FALSE(instr.is_imm);
    EXPECT_TRUE(instr.set_flags);
}

TEST(DecoderTest, DecodeThumb16CmpRegister) {
    // 0x4288 = CMP R0, R1 (Thumb 16-bit: 010000 1010 Rm(0001) Rn/Rd(0000))
    uint16_t raw_cmp = 0x4288;
    DecodedInstruction instr = Decoder::decode16(raw_cmp);

    EXPECT_EQ(instr.op, Opcode::CMP);
    EXPECT_EQ(instr.rn, 0u);
    EXPECT_EQ(instr.rm, 1u);
    EXPECT_TRUE(instr.set_flags);
}
