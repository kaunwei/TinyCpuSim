#include <gtest/gtest.h>
#include "platforms/01_basic_perf/topology.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::platform;

TEST(BasicPerfPlatformTest, ExecuteFibonacciWorkload) {
    BasicPerfPlatform platform;
    platform.load_workload(PERF_WORKLOAD_ELF);
    uint64_t steps = platform.run(50000);
    EXPECT_GT(steps, 0u);
}

TEST(BasicPerfPlatformTest, DeadlineCountsExecutedSteps) {
    BasicPerfPlatform platform;
    platform.bus().write16(0, 0x2007); // MOVS r0, #7
    platform.bus().write16(2, 0x3001); // ADDS r0, #1
    const auto result = platform.run_until(1, 1);
    EXPECT_EQ(result.reason, StopReason::Deadline);
    EXPECT_EQ(result.progress, 1u);
    EXPECT_EQ(platform.time(), 1u);
    EXPECT_EQ(platform.state().get_reg(0), 7u);
}

TEST(BasicPerfPlatformTest, HaltReportsOnlyActualProgressForEveryQuantum) {
    for (uint64_t quantum : {1u, 8u, 64u, 500u}) {
        BasicPerfPlatform platform;
        platform.bus().write16(0, 0x2007); // MOVS r0, #7
        platform.bus().write16(2, 0x3001); // ADDS r0, #1
        platform.bus().write16(4, 0xDF00); // SVC #0
        const auto result = platform.run_until(100, quantum);
        EXPECT_EQ(result.reason, StopReason::Halt);
        EXPECT_EQ(result.progress, 2u);
        EXPECT_EQ(platform.time(), 2u);
        EXPECT_EQ(platform.state().get_reg(0), 8u);
    }
}

TEST(BasicPerfPlatformTest, FaultDoesNotChargeTheFaultingStep) {
    BasicPerfPlatform platform;
    platform.bus().write16(0, 0x2007); // MOVS r0, #7
    platform.bus().write16(2, 0xDE00); // Undefined instruction
    const auto result = platform.run_until(100, 8);
    EXPECT_EQ(result.reason, StopReason::Fault);
    EXPECT_EQ(result.progress, 1u);
    EXPECT_EQ(platform.time(), 1u);
    EXPECT_EQ(platform.state().get_reg(0), 7u);
}

TEST(BasicPerfPlatformTest, RejectsZeroQuantumAndContinuesRelativeBudget) {
    BasicPerfPlatform platform;
    platform.bus().write16(0, 0x2007);
    platform.bus().write16(2, 0x3001);
    platform.bus().write16(4, 0xDF00);
    EXPECT_THROW(platform.run_until(3, 0), std::invalid_argument);
    EXPECT_EQ(platform.time(), 0u);
    EXPECT_EQ(platform.run(1), 1u);
    EXPECT_EQ(platform.run(10), 1u);
    EXPECT_EQ(platform.time(), 2u);
    EXPECT_EQ(platform.state().get_reg(0), 8u);
    EXPECT_THROW(platform.run_until(1, 1), std::invalid_argument);
}

TEST(BasicPerfPlatformTest, WorkloadAgreesAcrossQuanta) {
    uint64_t reference_steps = 0;
    uint32_t reference_result = 0;
    for (uint64_t quantum : {1u, 8u, 64u, 500u}) {
        BasicPerfPlatform platform;
        platform.load_workload(PERF_WORKLOAD_ELF);
        const auto result = platform.run_until(50000, quantum);
        ASSERT_EQ(result.reason, StopReason::Halt);
        ASSERT_GT(result.progress, 0u);
        EXPECT_EQ(platform.time(), result.progress);
        if (quantum == 1) {
            reference_steps = result.progress;
            reference_result = platform.state().get_reg(0);
        }
        EXPECT_EQ(result.progress, reference_steps);
        EXPECT_EQ(platform.state().get_reg(0), reference_result);
    }
}
