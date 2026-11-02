#include <gtest/gtest.h>
#include "tinyarmsim/cpu/instruction.hpp"
#include "tinyarmsim/cpu/disassembler.hpp"
#include "tinyarmsim/cpu/decoder.hpp"

using namespace tinyarmsim;

TEST(DisassemblerTest, DisassembleBasicMoveAndAlu) {
    DecodedInstruction mov_instr{};
    mov_instr.op = Opcode::MOV;
    mov_instr.rd = 1;
    mov_instr.is_imm = true;
    mov_instr.imm = 42;
    mov_instr.set_flags = true;
    EXPECT_EQ(Disassembler::disassemble(mov_instr), "movs r1, #42");

    DecodedInstruction add_instr{};
    add_instr.op = Opcode::ADD;
    add_instr.rd = 0;
    add_instr.rn = 1;
    add_instr.rm = 2;
    add_instr.set_flags = true;
    EXPECT_EQ(Disassembler::disassemble(add_instr), "adds r0, r1, r2");

    DecodedInstruction sub_imm_instr{};
    sub_imm_instr.op = Opcode::SUB;
    sub_imm_instr.rd = 3;
    sub_imm_instr.rn = 3;
    sub_imm_instr.is_imm = true;
    sub_imm_instr.imm = 10;
    sub_imm_instr.set_flags = true;
    EXPECT_EQ(Disassembler::disassemble(sub_imm_instr), "subs r3, r3, #10");
}

TEST(DisassemblerTest, DisassembleMemoryAndStack) {
    DecodedInstruction ldr_instr{};
    ldr_instr.op = Opcode::LDR;
    ldr_instr.rd = 2;
    ldr_instr.rn = 4;
    ldr_instr.is_imm = true;
    ldr_instr.imm = 8;
    EXPECT_EQ(Disassembler::disassemble(ldr_instr), "ldr r2, [r4, #8]");

    DecodedInstruction push_instr{};
    push_instr.op = Opcode::PUSH;
    push_instr.register_list = (1 << 4) | (1 << 5) | (1 << 14); // {r4, r5, lr}
    EXPECT_EQ(Disassembler::disassemble(push_instr), "push {r4, r5, lr}");

    DecodedInstruction pop_instr{};
    pop_instr.op = Opcode::POP;
    pop_instr.register_list = (1 << 4) | (1 << 5) | (1 << 15); // {r4, r5, pc}
    EXPECT_EQ(Disassembler::disassemble(pop_instr), "pop {r4, r5, pc}");
}

TEST(DisassemblerTest, DisassembleBranchesAndSystem) {
    DecodedInstruction bl_instr{};
    bl_instr.op = Opcode::BL;
    bl_instr.is_imm = true;
    bl_instr.imm = 0x00010040;
    EXPECT_EQ(Disassembler::disassemble(bl_instr), "bl 0x10040");

    DecodedInstruction bx_instr{};
    bx_instr.op = Opcode::BX;
    bx_instr.rm = 14; // lr
    EXPECT_EQ(Disassembler::disassemble(bx_instr), "bx lr");

    DecodedInstruction cbz_instr{};
    cbz_instr.op = Opcode::CBZ;
    cbz_instr.rn = 0;
    cbz_instr.imm = 0x00010076;
    EXPECT_EQ(Disassembler::disassemble(cbz_instr), "cbz r0, 0x10076");

    DecodedInstruction svc_instr{};
    svc_instr.op = Opcode::SVC;
    svc_instr.imm = 0;
    EXPECT_EQ(Disassembler::disassemble(svc_instr), "svc #0");

    DecodedInstruction nop_instr{};
    nop_instr.op = Opcode::NOP;
    EXPECT_EQ(Disassembler::disassemble(nop_instr), "nop");
}

TEST(DisassemblerTest, DecoderAttachesPrecomputedDisassembly) {
    // 0x210a -> movs r1, #10
    DecodedInstruction decoded16 = Decoder::decode16(0x210a, 0x1000);
    EXPECT_EQ(decoded16.raw_hex, 0x210au);
    EXPECT_EQ(decoded16.disasm, "movs r1, #10");

    // 0xbf00 -> nop
    DecodedInstruction nop_dec = Decoder::decode16(0xbf00, 0x1000);
    EXPECT_EQ(nop_dec.raw_hex, 0xbf00u);
    EXPECT_EQ(nop_dec.disasm, "nop");
}
