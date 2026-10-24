#include <gtest/gtest.h>
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/instruction.hpp"
#include "tinyarmsim/decoder.hpp"
#include "tinyarmsim/interpreter.hpp"

using namespace tinyarmsim;

class LoadStoreTest : public ::testing::Test {
protected:
    ArchitecturalState state;
    MemoryBus bus{1024 * 1024};
    IsaInterpreter interpreter{state, bus};
};

TEST_F(LoadStoreTest, SingleWordLoadAndStore) {
    state.set_reg(1, 0x1000); // Base address
    state.set_reg(2, 0xAABBCCDD);

    // STR R2, [R1, #0]
    DecodedInstruction str_op{};
    str_op.op = Opcode::STR;
    str_op.rd = 2;
    str_op.rn = 1;
    str_op.is_imm = true;
    str_op.imm = 0;
    str_op.mem_size = 4;
    str_op.instr_size = 2;

    interpreter.execute(str_op);
    EXPECT_EQ(bus.read32(0x1000), 0xAABBCCDDu);

    // LDR R0, [R1, #0]
    DecodedInstruction ldr_op{};
    ldr_op.op = Opcode::LDR;
    ldr_op.rd = 0;
    ldr_op.rn = 1;
    ldr_op.is_imm = true;
    ldr_op.imm = 0;
    ldr_op.mem_size = 4;
    ldr_op.instr_size = 2;

    interpreter.execute(ldr_op);
    EXPECT_EQ(state.get_reg(0), 0xAABBCCDDu);
}

TEST_F(LoadStoreTest, ByteAndHalfwordAccessWithExtensions) {
    state.set_reg(1, 0x2000);

    // Write a negative signed byte (0xFE = -2) and negative signed halfword (0xFFF0 = -16)
    bus.write8(0x2000, 0xFE);
    bus.write16(0x2002, 0xFFF0);

    // LDRB R0, [R1, #0] -> zero-extended (0x000000FE)
    DecodedInstruction ldrb_op{};
    ldrb_op.op = Opcode::LDRB;
    ldrb_op.rd = 0;
    ldrb_op.rn = 1;
    ldrb_op.is_imm = true;
    ldrb_op.imm = 0;
    ldrb_op.mem_size = 1;
    ldrb_op.is_signed_mem = false;
    interpreter.execute(ldrb_op);
    EXPECT_EQ(state.get_reg(0), 0x000000FEu);

    // LDRSB R0, [R1, #0] -> sign-extended (0xFFFFFFFE = -2)
    DecodedInstruction ldrsb_op{};
    ldrsb_op.op = Opcode::LDRSB;
    ldrsb_op.rd = 0;
    ldrsb_op.rn = 1;
    ldrsb_op.is_imm = true;
    ldrsb_op.imm = 0;
    ldrsb_op.mem_size = 1;
    ldrsb_op.is_signed_mem = true;
    interpreter.execute(ldrsb_op);
    EXPECT_EQ(state.get_reg(0), 0xFFFFFFFEu);

    // LDRH R0, [R1, #2] -> zero-extended (0x0000FFF0)
    DecodedInstruction ldrh_op{};
    ldrh_op.op = Opcode::LDRH;
    ldrh_op.rd = 0;
    ldrh_op.rn = 1;
    ldrh_op.is_imm = true;
    ldrh_op.imm = 2;
    ldrh_op.mem_size = 2;
    ldrh_op.is_signed_mem = false;
    interpreter.execute(ldrh_op);
    EXPECT_EQ(state.get_reg(0), 0x0000FFF0u);

    // LDRSH R0, [R1, #2] -> sign-extended (0xFFFFFFF0 = -16)
    DecodedInstruction ldrsh_op{};
    ldrsh_op.op = Opcode::LDRSH;
    ldrsh_op.rd = 0;
    ldrsh_op.rn = 1;
    ldrsh_op.is_imm = true;
    ldrsh_op.imm = 2;
    ldrsh_op.mem_size = 2;
    ldrsh_op.is_signed_mem = true;
    interpreter.execute(ldrsh_op);
    EXPECT_EQ(state.get_reg(0), 0xFFFFFFF0u);
}

TEST_F(LoadStoreTest, PushAndPopStackRegisters) {
    state.set_sp(0x8000);
    state.set_reg(0, 0x11111111);
    state.set_reg(1, 0x22222222);
    state.set_lr(0x33333333);

    // PUSH {R0, R1, LR} -> bitmask: (1 << 0) | (1 << 1) | (1 << 14)
    DecodedInstruction push_op{};
    push_op.op = Opcode::PUSH;
    push_op.register_list = (1 << 0) | (1 << 1) | (1 << 14);
    push_op.instr_size = 2;

    interpreter.execute(push_op);
    // Pushed 3 registers (12 bytes) -> SP = 0x8000 - 12 = 0x7FF4
    EXPECT_EQ(state.get_sp(), 0x7FF4u);
    EXPECT_EQ(bus.read32(0x7FF4), 0x11111111u);
    EXPECT_EQ(bus.read32(0x7FF8), 0x22222222u);
    EXPECT_EQ(bus.read32(0x7FFC), 0x33333333u);

    // Clear registers
    state.set_reg(0, 0);
    state.set_reg(1, 0);
    state.set_pc(0x1000);

    // POP {R0, R1, PC} -> bitmask: (1 << 0) | (1 << 1) | (1 << 15)
    DecodedInstruction pop_op{};
    pop_op.op = Opcode::POP;
    pop_op.register_list = (1 << 0) | (1 << 1) | (1 << 15);
    pop_op.instr_size = 2;

    interpreter.execute(pop_op);
    EXPECT_EQ(state.get_sp(), 0x8000u);
    EXPECT_EQ(state.get_reg(0), 0x11111111u);
    EXPECT_EQ(state.get_reg(1), 0x22222222u);
    EXPECT_EQ(state.get_pc(), 0x33333332u); // PC loaded from pop (cleared bit 0)
}

TEST_F(LoadStoreTest, LdmAndStmBlockTransfer) {
    state.set_reg(4, 0x4000); // Base pointer in R4
    state.set_reg(0, 10);
    state.set_reg(1, 20);
    state.set_reg(2, 30);

    // STM R4!, {R0, R1, R2}
    DecodedInstruction stm_op{};
    stm_op.op = Opcode::STM;
    stm_op.rn = 4;
    stm_op.register_list = (1 << 0) | (1 << 1) | (1 << 2);
    stm_op.writeback = true;
    stm_op.instr_size = 2;

    interpreter.execute(stm_op);
    EXPECT_EQ(bus.read32(0x4000), 10u);
    EXPECT_EQ(bus.read32(0x4004), 20u);
    EXPECT_EQ(bus.read32(0x4008), 30u);
    EXPECT_EQ(state.get_reg(4), 0x400Cu); // Base updated

    // Clear registers and LDM
    state.set_reg(0, 0);
    state.set_reg(1, 0);
    state.set_reg(2, 0);
    state.set_reg(4, 0x4000);

    // LDM R4!, {R0, R1, R2}
    DecodedInstruction ldm_op{};
    ldm_op.op = Opcode::LDM;
    ldm_op.rn = 4;
    ldm_op.register_list = (1 << 0) | (1 << 1) | (1 << 2);
    ldm_op.writeback = true;
    ldm_op.instr_size = 2;

    interpreter.execute(ldm_op);
    EXPECT_EQ(state.get_reg(0), 10u);
    EXPECT_EQ(state.get_reg(1), 20u);
    EXPECT_EQ(state.get_reg(2), 30u);
    EXPECT_EQ(state.get_reg(4), 0x400Cu);
}

// === Decoder Tests for Load/Store ===

TEST(DecoderLoadStoreTest, DecodeThumb16LdrStrImmediate) {
    // 0x6808 = LDR R0, [R1, #0] (Thumb 16-bit: 0110 1 imm5(00000) Rn(001) Rd(000))
    uint16_t raw_ldr = 0x6808;
    DecodedInstruction instr = Decoder::decode16(raw_ldr);

    EXPECT_EQ(instr.op, Opcode::LDR);
    EXPECT_EQ(instr.rd, 0u);
    EXPECT_EQ(instr.rn, 1u);
    EXPECT_TRUE(instr.is_imm);
    EXPECT_EQ(instr.imm, 0u);
    EXPECT_EQ(instr.mem_size, 4u);
}

TEST(DecoderLoadStoreTest, DecodeThumb16PushPop) {
    // 0xB503 = PUSH {R0, R1, LR} (Thumb 16-bit: 1011 0 10 M(1) rlist(00000011))
    uint16_t raw_push = 0xB503;
    DecodedInstruction push_instr = Decoder::decode16(raw_push);

    EXPECT_EQ(push_instr.op, Opcode::PUSH);
    EXPECT_EQ(push_instr.register_list, (1 << 0) | (1 << 1) | (1 << 14));

    // 0xBD03 = POP {R0, R1, PC} (Thumb 16-bit: 1011 1 10 P(1) rlist(00000011))
    uint16_t raw_pop = 0xBD03;
    DecodedInstruction pop_instr = Decoder::decode16(raw_pop);

    EXPECT_EQ(pop_instr.op, Opcode::POP);
    EXPECT_EQ(pop_instr.register_list, (1 << 0) | (1 << 1) | (1 << 15));
}

TEST(DecoderLoadStoreTest, DecodeThumb2MultipleTransferAddressingModes) {
    const auto stmdb = Decoder::decode32(0xE920, 0x0006);
    EXPECT_EQ(stmdb.op, Opcode::STM);
    EXPECT_EQ(stmdb.rn, 0u);
    EXPECT_EQ(stmdb.register_list, 0x0006u);
    EXPECT_TRUE(stmdb.writeback);
    EXPECT_FALSE(stmdb.add_offset);
    EXPECT_TRUE(stmdb.pre_indexed);
    EXPECT_EQ(stmdb.instr_size, 4u);

    const auto stmdb_no_writeback = Decoder::decode32(0xE900, 0x0106);
    EXPECT_EQ(stmdb_no_writeback.op, Opcode::STM);
    EXPECT_FALSE(stmdb_no_writeback.writeback);
    EXPECT_FALSE(stmdb_no_writeback.add_offset);

    const auto stmia = Decoder::decode32(0xE8A0, 0x0106);
    EXPECT_EQ(stmia.op, Opcode::STM);
    EXPECT_TRUE(stmia.writeback);
    EXPECT_TRUE(stmia.add_offset);

    const auto ldmia = Decoder::decode32(0xE8B0, 0x0106);
    EXPECT_EQ(ldmia.op, Opcode::LDM);
    EXPECT_TRUE(ldmia.is_load);
    EXPECT_TRUE(ldmia.writeback);
    EXPECT_TRUE(ldmia.add_offset);

    const auto push = Decoder::decode32(0xE92D, 0x4106);
    EXPECT_EQ(push.op, Opcode::PUSH);
    EXPECT_EQ(push.register_list, 0x4106u);
    const auto pop = Decoder::decode32(0xE8BD, 0x8106);
    EXPECT_EQ(pop.op, Opcode::POP);
    EXPECT_EQ(pop.register_list, 0x8106u);
}

TEST(DecoderLoadStoreTest, RejectsExclusiveAndDoublewordTransfers) {
    const auto expect_undefined = [](uint16_t first, uint16_t second) {
        EXPECT_THROW(Decoder::decode32(first, second), UndefinedInstructionException);
        ArchitecturalState state;
        MemoryBus bus{1024 * 1024};
        IsaInterpreter interpreter{state, bus};
        EXPECT_THROW(interpreter.execute(Decoder::decode32(first, second)), UndefinedInstructionException);
    };

    expect_undefined(0xE851, 0x0F00); // LDREX r0, [r1]
    expect_undefined(0xE9D0, 0x0102); // LDRD r0, r1, [r0]
}

TEST_F(LoadStoreTest, Thumb2MultipleTransfersHonorDirectionAndAscendingRegisters) {
    state.set_reg(0, 0x200);
    state.set_reg(1, 0x11111111);
    state.set_reg(2, 0x22222222);
    interpreter.execute(Decoder::decode32(0xE920, 0x0006));
    EXPECT_EQ(bus.read32(0x1F8), 0x11111111u);
    EXPECT_EQ(bus.read32(0x1FC), 0x22222222u);
    EXPECT_EQ(state.get_reg(0), 0x1F8u);

    state.set_reg(3, 0x300);
    state.set_reg(8, 0x88888888);
    state.set_reg(12, 0xCCCCCCCC);
    interpreter.execute(Decoder::decode32(0xE8A3, 0x1100));
    EXPECT_EQ(bus.read32(0x300), 0x88888888u);
    EXPECT_EQ(bus.read32(0x304), 0xCCCCCCCCu);
    EXPECT_EQ(state.get_reg(3), 0x308u);

    state.set_reg(3, 0x300);
    state.set_reg(8, 0);
    state.set_reg(12, 0);
    interpreter.execute(Decoder::decode32(0xE8B3, 0x1100));
    EXPECT_EQ(state.get_reg(8), 0x88888888u);
    EXPECT_EQ(state.get_reg(12), 0xCCCCCCCCu);
    EXPECT_EQ(state.get_reg(3), 0x308u);

    state.set_reg(3, 0x340);
    interpreter.execute(Decoder::decode32(0xE883, 0x1100));
    EXPECT_EQ(bus.read32(0x340), 0x88888888u);
    EXPECT_EQ(bus.read32(0x344), 0xCCCCCCCCu);
    EXPECT_EQ(state.get_reg(3), 0x340u);

    state.set_sp(0x500);
    state.set_reg(8, 0x88888888);
    state.set_lr(0xEEEEEEEE);
    interpreter.execute(Decoder::decode32(0xE92D, 0x4100));
    EXPECT_EQ(state.get_sp(), 0x4F8u);
    EXPECT_EQ(bus.read32(0x4F8), 0x88888888u);
    EXPECT_EQ(bus.read32(0x4FC), 0xEEEEEEEEu);
    state.set_reg(8, 0);
    state.set_lr(0);
    interpreter.execute(Decoder::decode32(0xE8BD, 0x4100));
    EXPECT_EQ(state.get_reg(8), 0x88888888u);
    EXPECT_EQ(state.get_lr(), 0xEEEEEEEEu);
    EXPECT_EQ(state.get_sp(), 0x500u);
}

TEST(DecoderLoadStoreTest, DecodeWideSignedByteImmediate12Load) {
    ArchitecturalState state;
    MemoryBus bus{1024 * 1024};
    IsaInterpreter interpreter{state, bus};
    state.set_reg(0, 0x1000);
    bus.write8(0x1001, 0x80);
    bus.write8(0x1003, 0x7F);
    state.set_flags(true, false, true, true);

    auto instruction = Decoder::decode32(0xF990, 0x1001);
    EXPECT_EQ(instruction.op, Opcode::LDRSB);
    EXPECT_EQ(instruction.mem_size, 1u);
    EXPECT_TRUE(instruction.is_signed_mem);
    interpreter.execute(instruction);
    EXPECT_EQ(state.get_reg(1), 0xFFFFFF80u);
    auto positive = Decoder::decode32(0xF990, 0x1003);
    interpreter.execute(positive);
    EXPECT_EQ(state.get_reg(1), 0x7Fu);
    const auto unsigned_byte = Decoder::decode32(0xF890, 0x1001);
    EXPECT_EQ(unsigned_byte.op, Opcode::LDRB);
    interpreter.execute(unsigned_byte);
    EXPECT_EQ(state.get_reg(1), 0x80u);
    EXPECT_TRUE(state.get_flag_n());
    EXPECT_FALSE(state.get_flag_z());
    EXPECT_TRUE(state.get_flag_c());
    EXPECT_TRUE(state.get_flag_v());
}

TEST(DecoderLoadStoreTest, DecodeWideSignedHalfwordImmediate12Load) {
    ArchitecturalState state;
    MemoryBus bus{1024 * 1024};
    IsaInterpreter interpreter{state, bus};
    state.set_reg(0, 0x1000);
    bus.write16(0x1002, 0x8001);
    bus.write16(0x1004, 0x7F01);
    state.set_flags(false, true, false, true);

    auto instruction = Decoder::decode32(0xF9B0, 0x1002);
    EXPECT_EQ(instruction.op, Opcode::LDRSH);
    EXPECT_EQ(instruction.mem_size, 2u);
    EXPECT_TRUE(instruction.is_signed_mem);
    interpreter.execute(instruction);
    EXPECT_EQ(state.get_reg(1), 0xFFFF8001u);
    auto positive = Decoder::decode32(0xF9B0, 0x1004);
    interpreter.execute(positive);
    EXPECT_EQ(state.get_reg(1), 0x7F01u);
    const auto unsigned_halfword = Decoder::decode32(0xF8B0, 0x1002);
    EXPECT_EQ(unsigned_halfword.op, Opcode::LDRH);
    interpreter.execute(unsigned_halfword);
    EXPECT_EQ(state.get_reg(1), 0x8001u);
    EXPECT_FALSE(state.get_flag_n());
    EXPECT_TRUE(state.get_flag_z());
    EXPECT_FALSE(state.get_flag_c());
    EXPECT_TRUE(state.get_flag_v());
}
