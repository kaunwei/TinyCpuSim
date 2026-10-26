#include <gtest/gtest.h>
#include <array>
#include <utility>
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/instruction.hpp"
#include "tinyarmsim/decoder.hpp"
#include "tinyarmsim/interpreter.hpp"

using namespace tinyarmsim;

class ExtendedDataProcessingTest : public ::testing::Test {
protected:
    ArchitecturalState state;
    MemoryBus bus{1024 * 1024};
    IsaInterpreter interpreter{state, bus};
};

// === Logical & Bitwise Tests ===

TEST_F(ExtendedDataProcessingTest, AndOrrEorBic) {
    state.set_reg(1, 0xF0F0AAAA);
    state.set_reg(2, 0x0F0F5555);

    // AND R0, R1, R2 -> 0x00000000 (Z flag set)
    DecodedInstruction and_op{};
    and_op.op = Opcode::AND;
    and_op.rd = 0;
    and_op.rn = 1;
    and_op.rm = 2;
    and_op.set_flags = true;
    interpreter.execute(and_op);
    EXPECT_EQ(state.get_reg(0), 0x00000000u);
    EXPECT_TRUE(state.get_flag_z());

    // ORR R0, R1, R2 -> 0xFFFFFFFF (N flag set)
    DecodedInstruction orr_op{};
    orr_op.op = Opcode::ORR;
    orr_op.rd = 0;
    orr_op.rn = 1;
    orr_op.rm = 2;
    orr_op.set_flags = true;
    interpreter.execute(orr_op);
    EXPECT_EQ(state.get_reg(0), 0xFFFFFFFFu);
    EXPECT_TRUE(state.get_flag_n());

    // EOR R0, R1, R2 -> 0xFFFFFFFF
    DecodedInstruction eor_op{};
    eor_op.op = Opcode::EOR;
    eor_op.rd = 0;
    eor_op.rn = 1;
    eor_op.rm = 2;
    interpreter.execute(eor_op);
    EXPECT_EQ(state.get_reg(0), 0xFFFFFFFFu);

    // BIC R0, R1, R2 (Bit Clear: R1 AND NOT R2) -> 0xF0F0AAAA AND 0xF0F0AAAA = 0xF0F0AAAA
    DecodedInstruction bic_op{};
    bic_op.op = Opcode::BIC;
    bic_op.rd = 0;
    bic_op.rn = 1;
    bic_op.rm = 2;
    interpreter.execute(bic_op);
    EXPECT_EQ(state.get_reg(0), 0xF0F0AAAAu);
}

TEST_F(ExtendedDataProcessingTest, TstAndTeqAndCmn) {
    state.set_reg(0, 0x80000000);
    state.set_reg(1, 0x00000001);

    // TST R0, R1 (Bitwise AND test) -> result 0 -> Z=1
    DecodedInstruction tst_op{};
    tst_op.op = Opcode::TST;
    tst_op.rn = 0;
    tst_op.rm = 1;
    tst_op.set_flags = true;
    interpreter.execute(tst_op);
    EXPECT_TRUE(state.get_flag_z());

    // TEQ R0, R0 (Test Equivalence: EOR) -> result 0 -> Z=1
    DecodedInstruction teq_op{};
    teq_op.op = Opcode::TEQ;
    teq_op.rn = 0;
    teq_op.rm = 0;
    teq_op.set_flags = true;
    interpreter.execute(teq_op);
    EXPECT_TRUE(state.get_flag_z());

    // CMN R0, R1 (Compare Negative: ADD Rn, Rm) -> 0x80000000 + 1 = 0x80000001 -> N=1, Z=0
    DecodedInstruction cmn_op{};
    cmn_op.op = Opcode::CMN;
    cmn_op.rn = 0;
    cmn_op.rm = 1;
    cmn_op.set_flags = true;
    interpreter.execute(cmn_op);
    EXPECT_TRUE(state.get_flag_n());
    EXPECT_FALSE(state.get_flag_z());
}

// === Arithmetic Tests: ADC, SBC, RSB, MUL, MLA ===

TEST_F(ExtendedDataProcessingTest, AdcAndSbcWithCarry) {
    state.set_reg(1, 10);
    state.set_reg(2, 20);
    state.set_flag_c(true); // Carry is 1

    // ADC R0, R1, R2 -> 10 + 20 + 1 = 31
    DecodedInstruction adc_op{};
    adc_op.op = Opcode::ADC;
    adc_op.rd = 0;
    adc_op.rn = 1;
    adc_op.rm = 2;
    interpreter.execute(adc_op);
    EXPECT_EQ(state.get_reg(0), 31u);

    // SBC R0, R2, R1 -> R2 - R1 - NOT(C) = 20 - 10 - 0 = 10 (with C=1)
    DecodedInstruction sbc_op{};
    sbc_op.op = Opcode::SBC;
    sbc_op.rd = 0;
    sbc_op.rn = 2;
    sbc_op.rm = 1;
    interpreter.execute(sbc_op);
    EXPECT_EQ(state.get_reg(0), 10u);

    // Now with C=0 -> SBC should subtract extra 1: 20 - 10 - 1 = 9
    state.set_flag_c(false);
    interpreter.execute(sbc_op);
    EXPECT_EQ(state.get_reg(0), 9u);
}

TEST_F(ExtendedDataProcessingTest, RsbReverseSubtract) {
    state.set_reg(1, 10);
    state.set_reg(2, 35);

    // RSB R0, R1, R2 -> R2 - R1 = 35 - 10 = 25
    DecodedInstruction rsb_op{};
    rsb_op.op = Opcode::RSB;
    rsb_op.rd = 0;
    rsb_op.rn = 1;
    rsb_op.rm = 2;
    interpreter.execute(rsb_op);
    EXPECT_EQ(state.get_reg(0), 25u);
}

TEST_F(ExtendedDataProcessingTest, ThumbNegSubtractsRegisterFromZero) {
    struct NegCase {
        uint16_t encoding;
        uint32_t input;
        uint32_t expected;
        bool n;
        bool z;
        bool c;
        bool v;
    };
    constexpr std::array cases{
        NegCase{0x4240, 0x00000000u, 0x00000000u, false, true, true, false},
        NegCase{0x4248, 0x00000080u, 0xFFFFFF80u, true, false, false, false},
        NegCase{0x4248, 0xFFFFFF80u, 0x00000080u, false, false, false, false},
        NegCase{0x4248, 0x80000000u, 0x80000000u, true, false, false, true},
    };

    for (const auto& test_case : cases) {
        state.reset();
        const uint8_t source = static_cast<uint8_t>((test_case.encoding >> 3) & 0x7);
        state.set_reg(source, test_case.input);
        state.set_flags(false, false, true, false);
        const auto instruction = Decoder::decode16(test_case.encoding, 0x100);
        interpreter.execute(instruction);
        EXPECT_EQ(state.get_reg(instruction.rd), test_case.expected);
        if (source != instruction.rd) {
            EXPECT_EQ(state.get_reg(source), test_case.input);
        }
        EXPECT_EQ(state.get_flag_n(), test_case.n);
        EXPECT_EQ(state.get_flag_z(), test_case.z);
        EXPECT_EQ(state.get_flag_c(), test_case.c);
        EXPECT_EQ(state.get_flag_v(), test_case.v);
    }
}

TEST_F(ExtendedDataProcessingTest, ThumbNegCanWriteDistinctDestination) {
    state.set_reg(0, 0xA5A5A5A5u);
    state.set_reg(1, 0x00000080u);

    const auto instruction = Decoder::decode16(0x4248, 0x200);
    EXPECT_EQ(instruction.op, Opcode::RSB);
    EXPECT_EQ(instruction.rd, 0u);
    EXPECT_TRUE(instruction.is_imm);
    EXPECT_EQ(instruction.imm, 0u);
    interpreter.execute(instruction);

    EXPECT_EQ(state.get_reg(0), 0xFFFFFF80u);
    EXPECT_EQ(state.get_reg(1), 0x00000080u);
}

TEST_F(ExtendedDataProcessingTest, ThumbNegStepAdvancesProgramCounterByTwo) {
    state.set_reg(1, 0x00000080u);
    state.set_pc(0x100);
    bus.write16(0x100, 0x4248);

    interpreter.step();

    EXPECT_EQ(state.get_reg(0), 0xFFFFFF80u);
    EXPECT_EQ(state.get_pc(), 0x102u);
}

TEST_F(ExtendedDataProcessingTest, MulAndMla) {
    state.set_reg(1, 6);
    state.set_reg(2, 7);
    state.set_reg(3, 10);

    // MUL R0, R1, R2 -> 6 * 7 = 42
    DecodedInstruction mul_op{};
    mul_op.op = Opcode::MUL;
    mul_op.rd = 0;
    mul_op.rn = 1;
    mul_op.rm = 2;
    interpreter.execute(mul_op);
    EXPECT_EQ(state.get_reg(0), 42u);

    // MLA R0, R1, R2, R3 -> (6 * 7) + 10 = 52
    DecodedInstruction mla_op{};
    mla_op.op = Opcode::MLA;
    mla_op.rd = 0;
    mla_op.rn = 1;
    mla_op.rm = 2;
    mla_op.rs = 3;
    interpreter.execute(mla_op);
    EXPECT_EQ(state.get_reg(0), 52u);
}

// === Barrel Shifter Tests ===

TEST_F(ExtendedDataProcessingTest, ShiftsLslLsrAsrRor) {
    state.set_reg(1, 0x80000008);

    // LSL R0, R1, #2 -> 0x00000020
    DecodedInstruction lsl_op{};
    lsl_op.op = Opcode::LSL;
    lsl_op.rd = 0;
    lsl_op.rn = 1;
    lsl_op.is_imm = true;
    lsl_op.imm = 2;
    interpreter.execute(lsl_op);
    EXPECT_EQ(state.get_reg(0), 0x00000020u);

    // LSR R0, R1, #3 -> 0x10000001 (logical shift right zeroes top)
    DecodedInstruction lsr_op{};
    lsr_op.op = Opcode::LSR;
    lsr_op.rd = 0;
    lsr_op.rn = 1;
    lsr_op.is_imm = true;
    lsr_op.imm = 3;
    interpreter.execute(lsr_op);
    EXPECT_EQ(state.get_reg(0), 0x10000001u);

    // ASR R0, R1, #3 -> 0xF0000001 (arithmetic shift right preserves sign bit)
    DecodedInstruction asr_op{};
    asr_op.op = Opcode::ASR;
    asr_op.rd = 0;
    asr_op.rn = 1;
    asr_op.is_imm = true;
    asr_op.imm = 3;
    interpreter.execute(asr_op);
    EXPECT_EQ(state.get_reg(0), 0xF0000001u);

    // ROR R0, R1, #4 -> 0x88000000
    DecodedInstruction ror_op{};
    ror_op.op = Opcode::ROR;
    ror_op.rd = 0;
    ror_op.rn = 1;
    ror_op.is_imm = true;
    ror_op.imm = 4;
    interpreter.execute(ror_op);
    EXPECT_EQ(state.get_reg(0), 0x88000000u);
}

TEST_F(ExtendedDataProcessingTest, NarrowShiftImmediateZeroMeansThirtyTwoExceptLslAlias) {
    const auto lsr = Decoder::decode16(0x0808);
    const auto asr = Decoder::decode16(0x1008);
    const auto lsl_alias = Decoder::decode16(0x0008);
    EXPECT_EQ(lsr.imm, 32u);
    EXPECT_EQ(asr.imm, 32u);
    EXPECT_EQ(lsl_alias.op, Opcode::MOV);
}

TEST_F(ExtendedDataProcessingTest, StandaloneShiftsProduceArchitecturalCarryAtBoundaries) {
    struct Case { Opcode op; uint32_t amount; uint32_t result; bool carry; };
    constexpr std::array cases{
        Case{Opcode::LSL, 0, 0x80000001u, false}, Case{Opcode::LSL, 1, 0x00000002u, true},
        Case{Opcode::LSL, 31, 0x80000000u, false}, Case{Opcode::LSL, 32, 0u, true},
        Case{Opcode::LSL, 33, 0u, false}, Case{Opcode::LSL, 255, 0u, false},
        Case{Opcode::LSL, 256, 0x80000001u, false},
        Case{Opcode::LSR, 0, 0x80000001u, false}, Case{Opcode::LSR, 1, 0x40000000u, true},
        Case{Opcode::LSR, 31, 1u, false}, Case{Opcode::LSR, 32, 0u, true},
        Case{Opcode::LSR, 33, 0u, false}, Case{Opcode::LSR, 255, 0u, false},
        Case{Opcode::LSR, 256, 0x80000001u, false},
        Case{Opcode::ASR, 0, 0x80000001u, false}, Case{Opcode::ASR, 1, 0xC0000000u, true},
        Case{Opcode::ASR, 31, 0xFFFFFFFFu, false}, Case{Opcode::ASR, 32, 0xFFFFFFFFu, true},
        Case{Opcode::ASR, 33, 0xFFFFFFFFu, true}, Case{Opcode::ASR, 255, 0xFFFFFFFFu, true},
        Case{Opcode::ASR, 256, 0x80000001u, false},
        Case{Opcode::ROR, 0, 0x80000001u, false}, Case{Opcode::ROR, 1, 0xC0000000u, true},
        Case{Opcode::ROR, 31, 3u, false}, Case{Opcode::ROR, 32, 0x80000001u, true},
        Case{Opcode::ROR, 33, 0xC0000000u, true}, Case{Opcode::ROR, 255, 3u, false},
        Case{Opcode::ROR, 256, 0x80000001u, false},
    };
    for (const auto& test_case : cases) {
        state.reset();
        state.set_reg(1, 0x80000001u);
        state.set_reg(2, test_case.amount);
        state.set_flags(false, false, false, true);
        DecodedInstruction instr{};
        instr.op = test_case.op;
        instr.rd = 0;
        instr.rn = 1;
        instr.rm = 2;
        instr.is_imm = false;
        instr.set_flags = true;
        interpreter.execute(instr);
        EXPECT_EQ(state.get_reg(0), test_case.result);
        EXPECT_EQ(state.get_flag_c(), test_case.carry);
        EXPECT_EQ(state.get_flag_v(), true);
    }
}

TEST_F(ExtendedDataProcessingTest, ZeroShiftPreservesBothIncomingCarryValues) {
    for (bool carry : {false, true}) {
        state.reset();
        state.set_reg(1, 0x80000001u);
        state.set_reg(2, 0);
        state.set_flag_c(carry);
        DecodedInstruction instr{};
        instr.op = Opcode::LSR; instr.rd = 0; instr.rn = 1; instr.rm = 2; instr.set_flags = true;
        interpreter.execute(instr);
        EXPECT_EQ(state.get_reg(0), 0x80000001u);
        EXPECT_EQ(state.get_flag_c(), carry);
    }
}

TEST_F(ExtendedDataProcessingTest, ShiftFlagWritesRespectNonSAndItPolicy) {
    state.set_reg(1, 0x80000001u);
    state.set_reg(2, 1);
    state.set_flags(false, false, false, true);
    DecodedInstruction instr{};
    instr.op = Opcode::LSR; instr.rd = 0; instr.rn = 1; instr.rm = 2; instr.is_imm = false;
    instr.set_flags = false;
    interpreter.execute(instr);
    EXPECT_EQ(state.get_reg(0), 0x40000000u);
    EXPECT_FALSE(state.get_flag_n()); EXPECT_FALSE(state.get_flag_c()); EXPECT_TRUE(state.get_flag_v());

    state.reset(); state.set_reg(1, 0x80000001u); state.set_reg(2, 1);
    state.set_flags(false, true, false, true); state.set_it_state(0x08);
    instr.is_imm = true; instr.imm = 1; instr.set_flags = true;
    interpreter.execute(instr);
    EXPECT_EQ(state.get_reg(0), 0x40000000u);
    EXPECT_FALSE(state.get_flag_n()); EXPECT_FALSE(state.get_flag_c()); EXPECT_TRUE(state.get_flag_v());
    EXPECT_EQ(state.get_it_state(), 0u);
}

// === Wide Immediate: MOVW / MOVT Tests ===

TEST_F(ExtendedDataProcessingTest, MovwAndMovtBuilds32BitAddress) {
    // MOVW R0, #0x5678 (loads lower 16 bits and zeroes upper 16 bits)
    DecodedInstruction movw_op{};
    movw_op.op = Opcode::MOVW;
    movw_op.rd = 0;
    movw_op.is_imm = true;
    movw_op.imm = 0x5678;
    movw_op.instr_size = 4;
    interpreter.execute(movw_op);
    EXPECT_EQ(state.get_reg(0), 0x00005678u);

    // MOVT R0, #0x1234 (loads upper 16 bits, keeping lower 16 bits intact)
    DecodedInstruction movt_op{};
    movt_op.op = Opcode::MOVT;
    movt_op.rd = 0;
    movt_op.is_imm = true;
    movt_op.imm = 0x1234;
    movt_op.instr_size = 4;
    interpreter.execute(movt_op);
    EXPECT_EQ(state.get_reg(0), 0x12345678u);
}

// === 32-bit Thumb Decoder Tests for MOVW & MOVT ===

TEST(DecoderTest, DecodeThumb32Movw) {
    // MOVW R0, #0x1234 -> 32-bit Thumb-2: 0xF241 0x2034
    // Encoding: 11110 i(0) 10 0 100 0000 0 imm3(001) Rd(0000) imm8(00110100) -> imm16 = 0x1234
    uint16_t w1 = 0xF241;
    uint16_t w2 = 0x2034;
    DecodedInstruction instr = Decoder::decode32(w1, w2);

    EXPECT_EQ(instr.op, Opcode::MOVW);
    EXPECT_EQ(instr.rd, 0u);
    EXPECT_TRUE(instr.is_imm);
    EXPECT_EQ(instr.imm, 0x1234u);
    EXPECT_EQ(instr.instr_size, 4u);
}

TEST(DecoderTest, DecodeThumb32Movt) {
    // MOVT R0, #0x5678 -> 32-bit Thumb-2: 0xF2C5 0x6078
    uint16_t w1 = 0xF2C5;
    uint16_t w2 = 0x6078;
    DecodedInstruction instr = Decoder::decode32(w1, w2);

    EXPECT_EQ(instr.op, Opcode::MOVT);
    EXPECT_EQ(instr.rd, 0u);
    EXPECT_TRUE(instr.is_imm);
    EXPECT_EQ(instr.imm, 0x5678u);
    EXPECT_EQ(instr.instr_size, 4u);
}

TEST_F(ExtendedDataProcessingTest, MovWideModifiedImmediatePreservesFlagsAndAdvancesPc) {
    struct Case {
        uint16_t first;
        uint16_t second;
        uint8_t destination;
        uint32_t value;
    };
    const std::array<Case, 10> cases{{
        {0xF04F, 0x0000, 0, 0x00000000u}, // assembler: MOV.W r0, #0
        {0xF04F, 0x4380, 3, 0x40000000u}, // assembler: MOV.W r3, #0x40000000
        {0xF04F, 0x4280, 2, 0x40000000u}, // assembler: MOV.W r2, #0x40000000
        {0xF04F, 0x0080, 0, 0x00000080u}, // ordinary 8-bit immediate
        {0xF04F, 0x1180, 1, 0x00800080u}, // byte replication
        {0xF04F, 0x2480, 4, 0x80008000u}, // byte replication
        {0xF04F, 0x3580, 5, 0x80808080u}, // byte replication
        {0xF04F, 0x4600, 6, 0x80000000u}, // rotated modified immediate
        {0xF04F, 0x4780, 7, 0x40000000u}, // rotated modified immediate
        {0xF44F, 0x0800, 8, 0x00800000u}, // rotated modified immediate
    }};
    const std::array<uint32_t, 4> nzcv_values{{0x00000000u, 0x50000000u, 0xA0000000u, 0xF0000000u}};

    for (const Case& test_case : cases) {
        const DecodedInstruction decoded = Decoder::decode32(test_case.first, test_case.second, 0x100u);
        EXPECT_EQ(decoded.op, Opcode::MOV);
        EXPECT_EQ(decoded.rd, test_case.destination);
        EXPECT_EQ(decoded.imm, test_case.value);
        EXPECT_TRUE(decoded.is_imm);
        EXPECT_FALSE(decoded.set_flags);
        EXPECT_EQ(decoded.instr_size, 4u);

        for (const uint32_t nzcv : nzcv_values) {
            state.set_flags((nzcv & 0x80000000u) != 0, (nzcv & 0x40000000u) != 0,
                            (nzcv & 0x20000000u) != 0, (nzcv & 0x10000000u) != 0);
            for (uint8_t reg = 0; reg < 15; ++reg) {
                state.set_reg(reg, 0xA5000000u | reg);
            }
            state.set_pc(0x100u);
            bus.write16(0x100u, test_case.first);
            bus.write16(0x102u, test_case.second);
            interpreter.step();

            EXPECT_EQ(state.get_reg(test_case.destination), test_case.value);
            for (uint8_t reg = 0; reg < 15; ++reg) {
                if (reg != test_case.destination) {
                    EXPECT_EQ(state.get_reg(reg), 0xA5000000u | reg);
                }
            }
            EXPECT_EQ(state.get_cpsr() & 0xF0000000u, nzcv);
            EXPECT_EQ(state.get_pc(), 0x104u);
        }
    }
}

TEST_F(ExtendedDataProcessingTest, DecodeAndExecuteCmnWideModifiedImmediate) {
    const std::array<std::pair<uint16_t, uint16_t>, 2> encodings{{
        {0xF113, 0x0F80}, {0xF110, 0x0F80}}};
    for (const auto& [w1, w2] : encodings) {
        const DecodedInstruction instr = Decoder::decode32(w1, w2);
        EXPECT_EQ(instr.op, Opcode::CMN);
        EXPECT_EQ(instr.rn, (w1 & 0xFu));
        EXPECT_EQ(instr.imm, 128u);
        EXPECT_TRUE(instr.is_imm);
        EXPECT_TRUE(instr.set_flags);
        EXPECT_EQ(instr.instr_size, 4u);

        state.set_reg(0, 0xA5A5A5A5u);
        state.set_reg(1, 0xFFFFFF80u);
        state.set_reg(2, 0xFFFFFF81u);
        state.set_reg(3, 0x12345678u);
        state.set_reg(4, 0x7FFFFF80u);
        state.set_reg(5, 0xFFFFFFFFu);

        const uint8_t source = instr.rn;
        const std::array<std::pair<uint32_t, uint32_t>, 4> cases{{
            {0xFFFFFF80u, 0x60000000u}, // -128 + 128 = 0, Z and C
            {0xFFFFFF81u, 0x20000000u}, // -127 + 128 = 1, carry
            {0x7FFFFF80u, 0x90000000u}, // 0x7fffff80 + 128 = 0x80000000, N and V
            {0xFFFFFFFFu, 0x20000000u}}}; // 0xffffffff + 128 = 0x7f, unsigned carry
        for (const auto& [operand, expected_nzcv] : cases) {
            state.set_reg(source, operand);
            state.set_flags(false, false, false, false);
            state.set_pc(0x1000u);
            const auto before = std::array<uint32_t, 15>{
                state.get_reg(0), state.get_reg(1), state.get_reg(2), state.get_reg(3),
                state.get_reg(4), state.get_reg(5), state.get_reg(6), state.get_reg(7),
                state.get_reg(8), state.get_reg(9), state.get_reg(10), state.get_reg(11),
                state.get_reg(12), state.get_reg(13), state.get_reg(14)};
            interpreter.execute(instr);
            EXPECT_EQ(state.get_pc(), 0x1004u);
            for (size_t reg = 0; reg < before.size(); ++reg) {
                EXPECT_EQ(state.get_reg(reg), before[reg]);
            }
            EXPECT_EQ(state.get_cpsr() & 0xF0000000u, expected_nzcv);
            EXPECT_EQ(interpreter.evaluate_condition(ConditionCode::EQ), (expected_nzcv & 0x40000000u) != 0);
            EXPECT_EQ(interpreter.evaluate_condition(ConditionCode::NE), (expected_nzcv & 0x40000000u) == 0);
        }
    }
}

TEST(DecoderTest, ModifiedImmediateExpansionPatternsAndUnsupportedNeighbors) {
    const std::array<std::pair<uint16_t, uint16_t>, 4> expansion_cases{{
        {0xF110, 0x0F80}, // #0x80
        {0xF110, 0x1F80}, // #0x00800080
        {0xF110, 0x2F80}, // #0x80008000
        {0xF110, 0x3F80}}}; // #0x80808080
    const std::array<uint32_t, 4> expected{{0x00000080u, 0x00800080u, 0x80008000u, 0x80808080u}};
    for (size_t i = 0; i < expansion_cases.size(); ++i) {
        const auto instr = Decoder::decode32(expansion_cases[i].first, expansion_cases[i].second);
        EXPECT_EQ(instr.op, Opcode::CMN);
        EXPECT_EQ(instr.imm, expected[i]);
    }

    const std::array<std::pair<uint16_t, uint16_t>, 3> rotated_cases{{
        {0xF110, 0x4F00}, {0xF110, 0x4F80}, {0xF510, 0x0F00}}};
    const std::array<uint32_t, 3> rotated_expected{{0x80000000u, 0x40000000u, 0x00800000u}};
    for (size_t i = 0; i < rotated_cases.size(); ++i) {
        const auto instr = Decoder::decode32(rotated_cases[i].first, rotated_cases[i].second);
        EXPECT_EQ(instr.op, Opcode::CMN);
        EXPECT_EQ(instr.imm, rotated_expected[i]);
    }

    EXPECT_THROW(Decoder::decode32(0xF190, 0x0F80), UndefinedInstructionException);
    EXPECT_THROW(Decoder::decode32(0xF130, 0x0F80), UndefinedInstructionException);
}

TEST_F(ExtendedDataProcessingTest, UnsupportedCmnNeighborEncodingsFaultAtPublicSeams) {
    const std::array<std::pair<uint16_t, uint16_t>, 2> neighbors{{
        {0xF310, 0x0F80}, // bit 9 set: undefined modified-immediate class
        {0xF100, 0x0F80}}}; // ADD.W PC, R0, #128 with S=0 is unsupported

    for (const auto& [w1, w2] : neighbors) {
        EXPECT_THROW(Decoder::decode32(w1, w2), UndefinedInstructionException);

        bus.write16(0, w1);
        bus.write16(2, w2);
        state.set_pc(0);
        EXPECT_THROW(interpreter.step(), UndefinedInstructionException);
    }
}

TEST_F(ExtendedDataProcessingTest, AndWideModifiedImmediatePreservesFlagsAndAdvancesPc) {
    struct Case {
        uint16_t first;
        uint16_t second;
        uint8_t source;
        uint8_t destination;
        uint32_t source_value;
        uint32_t immediate;
        uint32_t result;
    };
    const std::array<Case, 10> cases{{
        {0xF000, 0x000F, 0, 0, 0xA5A5A5A5u, 0x0000000Fu, 0x00000005u},
        {0xF001, 0x000F, 1, 0, 0x12345678u, 0x0000000Fu, 0x00000008u},
        {0xF004, 0x040F, 4, 4, 0xDEADBEEFu, 0x0000000Fu, 0x0000000Fu},
        {0xF004, 0x0100, 4, 1, 0xFFFFFFFFu, 0x00000000u, 0x00000000u},
        {0xF002, 0x0280, 2, 2, 0x123456FFu, 0x00000080u, 0x00000080u},
        {0xF003, 0x1380, 3, 3, 0xFFFFFFFFu, 0x00800080u, 0x00800080u},
        {0xF005, 0x2580, 5, 5, 0xFFFFFFFFu, 0x80008000u, 0x80008000u},
        {0xF006, 0x3680, 6, 6, 0xFFFFFFFFu, 0x80808080u, 0x80808080u},
        {0xF007, 0x4700, 7, 7, 0xFFFFFFFFu, 0x80000000u, 0x80000000u},
        {0xF008, 0x4880, 8, 8, 0xFFFFFFFFu, 0x40000000u, 0x40000000u},
    }};
    const std::array<uint32_t, 4> nzcv_values{{0x00000000u, 0x50000000u, 0xA0000000u, 0xF0000000u}};

    for (const Case& test_case : cases) {
        for (const uint32_t nzcv : nzcv_values) {
            for (uint8_t reg = 0; reg < 15; ++reg) {
                state.set_reg(reg, 0xA5000000u | reg);
            }
            state.set_reg(test_case.source, test_case.source_value);
            state.set_flags((nzcv & 0x80000000u) != 0, (nzcv & 0x40000000u) != 0,
                            (nzcv & 0x20000000u) != 0, (nzcv & 0x10000000u) != 0);
            state.set_pc(0x100u);
            bus.write16(0x100u, test_case.first);
            bus.write16(0x102u, test_case.second);

            interpreter.step();

            EXPECT_EQ(state.get_reg(test_case.destination), test_case.result);
            for (uint8_t reg = 0; reg < 15; ++reg) {
                if (reg != test_case.destination) {
                    const uint32_t expected = reg == test_case.source
                        ? test_case.source_value : (0xA5000000u | reg);
                    EXPECT_EQ(state.get_reg(reg), expected);
                }
            }
            EXPECT_EQ(state.get_cpsr() & 0xF0000000u, nzcv);
            EXPECT_EQ(state.get_pc(), 0x104u);
        }
    }
}
