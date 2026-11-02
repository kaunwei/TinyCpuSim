#include <gtest/gtest.h>
#include "tinyarmsim/cpu/decoder.hpp"
#include "tinyarmsim/uarch/uop_decoder.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::uarch;

TEST(UOpDecoderTest, StoreInstructionSplitsIntoDecoupledStaAndStd) {
    // STR R0, [R1, #8] (Thumb-16: 0x6088)
    DecodedInstruction dec = Decoder::decode16(0x6088, 0x1000);
    EXPECT_EQ(dec.op, Opcode::STR);

    std::vector<UOp> uops = UOpDecoder::decode(dec, 0x1000, 1);
    ASSERT_EQ(uops.size(), 2);

    // UOp 0: Store Address Generation (STA)
    const auto& uop_sta = uops[0];
    EXPECT_EQ(uop_sta.type, UOpType::STORE_ADDR);
    EXPECT_EQ(uop_sta.target_port, ExecutionPort::PORT_3_DUAL_AGU);
    EXPECT_EQ(uop_sta.arch_src1, 1); // R1
    EXPECT_EQ(uop_sta.imm, 8);
    EXPECT_EQ(uop_sta.mem_size_bytes, 4);

    // UOp 1: Store Data Generation (STD)
    const auto& uop_std = uops[1];
    EXPECT_EQ(uop_std.type, UOpType::STORE_DATA);
    EXPECT_EQ(uop_std.target_port, ExecutionPort::PORT_4_STORE_DATA);
    EXPECT_EQ(uop_std.arch_src1, 0); // R0
    EXPECT_EQ(uop_std.mem_size_bytes, 4);
}

TEST(UOpDecoderTest, LoadInstructionGeneratesSingleLoadUop) {
    // LDR R2, [R3, #4] (Thumb-16: 0x685A)
    DecodedInstruction dec = Decoder::decode16(0x685A, 0x1004);
    EXPECT_EQ(dec.op, Opcode::LDR);

    std::vector<UOp> uops = UOpDecoder::decode(dec, 0x1004, 2);
    ASSERT_EQ(uops.size(), 1);

    const auto& uop = uops[0];
    EXPECT_EQ(uop.type, UOpType::LOAD);
    EXPECT_EQ(uop.target_port, ExecutionPort::PORT_2_LOAD_AGU);
    EXPECT_EQ(uop.arch_dest, 2); // R2
    EXPECT_EQ(uop.arch_src1, 3); // R3
    EXPECT_EQ(uop.imm, 4);
    EXPECT_EQ(uop.mem_size_bytes, 4);
}

TEST(UOpDecoderTest, PushInstructionExpandsIntoMultiStoreUops) {
    // PUSH {R0, R1, LR} (Thumb-16: 0xB503)
    DecodedInstruction dec = Decoder::decode16(0xB503, 0x1008);
    EXPECT_EQ(dec.op, Opcode::PUSH);

    std::vector<UOp> uops = UOpDecoder::decode(dec, 0x1008, 3);
    // 1 SUB SP uop + 3 registers * 2 uops (STA + STD) = 7 uops
    ASSERT_EQ(uops.size(), 7);

    EXPECT_EQ(uops[0].type, UOpType::ALU);
    EXPECT_EQ(uops[0].opcode, Opcode::SUB);
    EXPECT_EQ(uops[0].arch_dest, 13);
    EXPECT_EQ(uops[0].imm, 12);

    EXPECT_EQ(uops[1].type, UOpType::STORE_ADDR);
    EXPECT_EQ(uops[2].type, UOpType::STORE_DATA);
    EXPECT_EQ(uops[2].arch_src1, 0); // R0

    EXPECT_EQ(uops[3].type, UOpType::STORE_ADDR);
    EXPECT_EQ(uops[4].type, UOpType::STORE_DATA);
    EXPECT_EQ(uops[4].arch_src1, 1); // R1

    EXPECT_EQ(uops[5].type, UOpType::STORE_ADDR);
    EXPECT_EQ(uops[6].type, UOpType::STORE_DATA);
    EXPECT_EQ(uops[6].arch_src1, 14); // LR
}

TEST(UOpDecoderTest, BranchAndCallInstructionClassification) {
    // BL target (Thumb-32: 0xF000 0xF800)
    DecodedInstruction dec_bl = Decoder::decode32(0xF000, 0xF800, 0x1010);
    EXPECT_EQ(dec_bl.op, Opcode::BL);

    auto uops_bl = UOpDecoder::decode(dec_bl, 0x1010, 4);
    ASSERT_EQ(uops_bl.size(), 1);
    EXPECT_EQ(uops_bl[0].type, UOpType::CALL);
    EXPECT_TRUE(uops_bl[0].is_branch);
    EXPECT_EQ(uops_bl[0].arch_dest, 14); // Writes LR

    // BX LR (Thumb-16: 0x4770)
    DecodedInstruction dec_bx = Decoder::decode16(0x4770, 0x1020);
    EXPECT_EQ(dec_bx.op, Opcode::BX);

    auto uops_bx = UOpDecoder::decode(dec_bx, 0x1020, 5);
    ASSERT_EQ(uops_bx.size(), 1);
    EXPECT_EQ(uops_bx[0].type, UOpType::RET);
    EXPECT_TRUE(uops_bx[0].is_branch);
    EXPECT_EQ(uops_bx[0].arch_src1, 14); // Reads LR
}

TEST(UOpDecoderTest, MultiplyAndDividePortRouting) {
    // MULS R0, R1, R0 (Thumb-16: 0x4348)
    DecodedInstruction dec_mul = Decoder::decode16(0x4348, 0x1030);
    EXPECT_EQ(dec_mul.op, Opcode::MUL);

    auto uops_mul = UOpDecoder::decode(dec_mul, 0x1030, 6);
    ASSERT_EQ(uops_mul.size(), 1);
    EXPECT_EQ(uops_mul[0].type, UOpType::MUL);
    EXPECT_EQ(uops_mul[0].target_port, ExecutionPort::PORT_1_ALU_MUL);
    EXPECT_EQ(uops_mul[0].arch_dest, 0);
    EXPECT_EQ(uops_mul[0].arch_src1, 0);
    EXPECT_EQ(uops_mul[0].arch_src2, 1);
}
