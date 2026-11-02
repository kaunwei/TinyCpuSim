#include <gtest/gtest.h>
#include <fstream>
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/memory/loader.hpp"
#include "tinyarmsim/uarch/ooo_core.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::uarch;

TEST(OoOCoreTest, ExecutesLinearInstructionSequenceAndRetires) {
    MemoryBus bus(64 * 1024);

    // 0x1000: MOV R0, #10 (Thumb-16: 0x200A)
    bus.write16(0x1000, 0x200A);
    // 0x1002: MOV R1, #20 (Thumb-16: 0x2114)
    bus.write16(0x1002, 0x2114);
    // 0x1004: ADD R2, R0, R1 (Thumb-16: 0x1842)
    bus.write16(0x1004, 0x1842);
    // 0x1006: SVC #0 (Halt) (Thumb-16: 0xDF00)
    bus.write16(0x1006, 0xDF00);

    CoreConfig cfg;
    cfg.fetch_width = 4;
    cfg.commit_width = 4;
    cfg.branch_predictor.type = PredictorType::NONE;

    OoOCore core(0, cfg, bus, nullptr, nullptr, 0x1000);

    for (int cycle = 0; cycle < 30; ++cycle) {
        core.tick();
        if (core.is_halted()) break;
    }

    EXPECT_TRUE(core.is_halted());
    EXPECT_GE(core.get_committed_instructions(), 3);
    EXPECT_GT(core.get_cycles(), 0);

    CoreStats stats = core.get_stats();
    EXPECT_GT(stats.ipc(), 0.0);
}

TEST(OoOCoreTest, NarrowShiftImmediateThirtyTwoFlowsThroughPublicPipeline) {
    MemoryBus bus(64 * 1024);
    bus.write16(0x1000, 0x2101); // MOVS R1, #1
    bus.write16(0x1002, 0x07C9); // LSLS R1, R1, #31
    bus.write16(0x1004, 0x3101); // ADDS R1, #1 -> 0x80000001
    bus.write16(0x1006, 0x0808); // LSRS R0, R1, #32
    bus.write16(0x1008, 0x100A); // ASRS R2, R1, #32
    bus.write16(0x100A, 0xDF00); // SVC #0

    CoreConfig cfg;
    cfg.fetch_width = 1;
    cfg.issue_width = 1;
    cfg.commit_width = 1;
    cfg.branch_predictor.type = PredictorType::NONE;
    OoOCore core(0, cfg, bus, nullptr, nullptr, 0x1000);
    for (int cycle = 0; cycle < 30 && !core.is_halted(); ++cycle) core.tick();

    EXPECT_TRUE(core.is_halted());
    EXPECT_EQ(core.read_arch_reg(0), 0u);
    EXPECT_EQ(core.read_arch_reg(2), 0xFFFFFFFFu);
    EXPECT_EQ(core.get_cycles(), 15u);
    EXPECT_EQ(core.get_committed_instructions(), 6u);
}

TEST(OoOCoreTest, RawDataHazardWakeupThroughPhysicalRegisters) {
    MemoryBus bus(64 * 1024);

    // 0x1000: MOV R0, #100 (Thumb-16: 0x2064)
    bus.write16(0x1000, 0x2064);
    // 0x1002: ADD R1, R0, #5 (Thumb-16: 0x3005) -> RAW Hazard on R0!
    bus.write16(0x1002, 0x3005);
    // 0x1004: SVC #0 (Thumb-16: 0xDF00)
    bus.write16(0x1004, 0xDF00);

    CoreConfig cfg;
    OoOCore core(0, cfg, bus, nullptr, nullptr, 0x1000);

    for (int cycle = 0; cycle < 20; ++cycle) {
        core.tick();
        if (core.is_halted()) break;
    }

    EXPECT_TRUE(core.is_halted());
    EXPECT_GE(core.get_committed_instructions(), 2);
}

TEST(OoOCoreTest, RunsTestArithmeticElfToCleanHalt) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    std::ifstream file("tests/fixtures/test_arithmetic.elf", std::ios::binary);
    if (!file.is_open()) {
        file.open("../tests/fixtures/test_arithmetic.elf", std::ios::binary);
    }
    ASSERT_TRUE(file.is_open());
    Loader::load_elf(file, bus, state);

    CoreConfig cfg;
    cfg.fetch_width = 2;
    cfg.commit_width = 2;
    cfg.issue_width = 2;
    cfg.rob_size = 32;
    cfg.rs_size = 16;
    OoOCore core(0, cfg, bus, nullptr, nullptr, state.get_pc());

    for (int cycle = 0; cycle < 10000; ++cycle) {
        core.tick();
        if (core.is_halted()) break;
    }

    std::cout << "DEBUG: TestArithmetic halted=" << core.is_halted() 
              << " committed=" << core.get_committed_instructions()
              << " cycles=" << core.get_cycles() << std::endl;

    EXPECT_TRUE(core.is_halted());
    EXPECT_GT(core.get_committed_instructions(), 1000);
}

TEST(OoOCoreTest, RunsTestFibonacciElfToCleanHalt) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    std::ifstream file("tests/fixtures/test_fibonacci.elf", std::ios::binary);
    if (!file.is_open()) {
        file.open("../tests/fixtures/test_fibonacci.elf", std::ios::binary);
    }
    ASSERT_TRUE(file.is_open());
    Loader::load_elf(file, bus, state);

    CoreConfig cfg;
    cfg.fetch_width = 4;
    cfg.commit_width = 4;
    cfg.issue_width = 4;
    cfg.rob_size = 64;
    cfg.rs_size = 32;
    OoOCore core(0, cfg, bus, nullptr, nullptr, state.get_pc());

    for (int cycle = 0; cycle < 10000; ++cycle) {
        try {
            core.tick();
        } catch (const std::exception& e) {
            std::cout << "DEBUG: Exception at cycle " << cycle << ": " << e.what() << std::endl;
            break;
        }
        if (core.is_halted()) break;
    }

    std::cout << "DEBUG: TestFibonacci halted=" << core.is_halted() 
              << " committed=" << core.get_committed_instructions()
              << " cycles=" << core.get_cycles() << std::endl;

    EXPECT_TRUE(core.is_halted());
    EXPECT_GT(core.get_committed_instructions(), 1000);
}

TEST(OoOCoreTest, RunsTestStoreForwardElfToCleanHalt) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    std::ifstream file("tests/fixtures/test_store_forward.elf", std::ios::binary);
    if (!file.is_open()) {
        file.open("../tests/fixtures/test_store_forward.elf", std::ios::binary);
    }
    ASSERT_TRUE(file.is_open());
    Loader::load_elf(file, bus, state);

    CoreConfig cfg;
    OoOCore core(0, cfg, bus, nullptr, nullptr, state.get_pc());

    for (int cycle = 0; cycle < 10000; ++cycle) {
        core.tick();
        if (core.is_halted()) break;
    }

    std::cout << "DEBUG: TestStoreForward halted=" << core.is_halted() 
              << " committed=" << core.get_committed_instructions()
              << " cycles=" << core.get_cycles() << std::endl;

    EXPECT_TRUE(core.is_halted());
    EXPECT_GT(core.get_committed_instructions(), 10);
}

TEST(OoOCoreTest, RunsTestStressElfToCleanHalt) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    std::ifstream file("tests/fixtures/test_stress.elf", std::ios::binary);
    if (!file.is_open()) {
        file.open("../tests/fixtures/test_stress.elf", std::ios::binary);
    }
    ASSERT_TRUE(file.is_open());
    Loader::load_elf(file, bus, state);

    CoreConfig cfg;
    OoOCore core(0, cfg, bus, nullptr, nullptr, state.get_pc());

    for (int cycle = 0; cycle < 200000; ++cycle) {
        core.tick();
        if (core.is_halted()) break;
    }

    std::cout << "DEBUG: TestStress halted=" << core.is_halted() 
              << " committed=" << core.get_committed_instructions()
              << " cycles=" << core.get_cycles() << std::endl;

    EXPECT_TRUE(core.is_halted());
    EXPECT_GT(core.get_committed_instructions(), 100000);
}

TEST(OoOCoreTest, RunsTestIsaCoverageElfToCleanHalt) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    std::ifstream file("tests/fixtures/test_isa_coverage.elf", std::ios::binary);
    if (!file.is_open()) {
        file.open("../tests/fixtures/test_isa_coverage.elf", std::ios::binary);
    }
    ASSERT_TRUE(file.is_open());
    Loader::load_elf(file, bus, state);

    CoreConfig cfg;
    OoOCore core(0, cfg, bus, nullptr, nullptr, state.get_pc());

    for (int cycle = 0; cycle < 10000; ++cycle) {
        core.tick();
        if (core.is_halted()) break;
    }

    std::cout << "DEBUG: TestIsaCoverage halted=" << core.is_halted() 
              << " committed=" << core.get_committed_instructions()
              << " cycles=" << core.get_cycles() << std::endl;

    EXPECT_TRUE(core.is_halted());
    EXPECT_GT(core.get_committed_instructions(), 100);
}

