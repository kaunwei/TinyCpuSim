#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "tinyarmsim/cpu/state.hpp"
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/memory/loader.hpp"
#include "tinyarmsim/cpu/interpreter.hpp"

using namespace tinyarmsim;

class AssemblySelfTest : public ::testing::Test {
protected:
    MemoryBus bus{64 * 1024 * 1024}; // 64MB flat RAM
    ArchitecturalState state;
    IsaInterpreter interpreter{state, bus};

    void run_elf_fixture(const std::string& fixture_path, uint32_t expected_exit_code = 0) {
        std::ifstream file(fixture_path, std::ios::binary);
        ASSERT_TRUE(file.is_open()) << "Failed to open fixture: " << fixture_path;

        Loader::load_elf(file, bus, state);
        EXPECT_EQ(state.get_pc(), 0x00010000u);

        uint32_t exit_code = interpreter.run(500000);
        EXPECT_EQ(exit_code, expected_exit_code);
    }
};

TEST_F(AssemblySelfTest, ExecuteArithmeticAssemblySelfTest) {
    run_elf_fixture("tests/fixtures/test_arithmetic.elf", 0);
}

TEST_F(AssemblySelfTest, ExecuteFibonacciAssemblySelfTest) {
    run_elf_fixture("tests/fixtures/test_fibonacci.elf", 0);
}

TEST_F(AssemblySelfTest, ExecuteSortAssemblySelfTest) {
    run_elf_fixture("tests/fixtures/test_sort.elf", 0);
}

TEST_F(AssemblySelfTest, ExecuteIsaCoverageAssemblySelfTest) {
    run_elf_fixture("tests/fixtures/test_isa_coverage.elf", 0);
}

TEST_F(AssemblySelfTest, ExecuteStressAssemblySelfTest) {
    std::ifstream file("tests/fixtures/test_stress.elf", std::ios::binary);
    ASSERT_TRUE(file.is_open());
    Loader::load_elf(file, bus, state);
    uint32_t exit_code = interpreter.run(1000000000);
    EXPECT_EQ(exit_code, 0u);
}
