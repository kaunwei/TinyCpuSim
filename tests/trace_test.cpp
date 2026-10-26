#include <gtest/gtest.h>
#include <iostream>
#include <sstream>
#include "tinyarmsim/trace.hpp"
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/interpreter.hpp"

using namespace tinyarmsim;

TEST(TraceTest, FormatSingleLineSpikeCommit) {
    TraceRecord record{};
    record.core_id = 0;
    record.pc = 0x00010014;
    record.raw_hex = 0x210a;
    record.instr_size = 2;
    record.disasm = "movs r1, #10";
    record.reg_writes.push_back({1, 0x0000000a});
    record.flag_n = false;
    record.flag_z = false;
    record.flag_c = false;
    record.flag_v = false;

    std::string line = record.format();
    EXPECT_NE(line.find("core 0: 0x00010014 (0x210a)"), std::string::npos);
    EXPECT_NE(line.find("movs r1, #10"), std::string::npos);
    EXPECT_NE(line.find("r1 0x0000000a"), std::string::npos);
    EXPECT_NE(line.find("NZCV=[0000]"), std::string::npos);
}

TEST(TraceTest, FormatMemoryAccessInCommit) {
    TraceRecord record{};
    record.core_id = 0;
    record.pc = 0x0001007e;
    record.raw_hex = 0x6021;
    record.instr_size = 2;
    record.disasm = "str r1, [r4, #0]";
    record.mem_accesses.push_back({true, 0x00020000, 4, 0x0000000f});
    record.flag_n = false;
    record.flag_z = true;

    std::string line = record.format();
    EXPECT_NE(line.find("mem[0x20000] <= 0xf (4B)"), std::string::npos);
    EXPECT_NE(line.find("NZCV=[0100]"), std::string::npos);
}

TEST(TraceTest, SkippedSlotKeepsOriginalPcRawWidthAndConditionNote) {
    ArchitecturalState state;
    MemoryBus bus;
    IsaInterpreter interpreter(state, bus);
    state.set_pc(0x1000);
    state.set_it_state(0x08);
    state.set_flag_z(false);
    bus.write16(0x1000, 0xDE42);
    interpreter.set_logging(true);
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());

    interpreter.step();

    std::cout.rdbuf(previous);
    EXPECT_NE(output.str().find("0x00001000 (0xde42)"), std::string::npos);
    EXPECT_NE(output.str().find("[condition false]"), std::string::npos);
    EXPECT_NE(output.str().find("NZCV=[0000]"), std::string::npos);
}
