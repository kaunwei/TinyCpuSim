#include <gtest/gtest.h>
#include "platforms/03_coprocessor_accel/topology.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::platform;

TEST(CoprocessorAccelPlatformTest, LiveConvolutionCoSimulation) {
    McuConvPlatform platform;
    platform.load_host_firmware(CONV_HOST_ELF);
    platform.load_accel_firmware(CONV_ACCEL_ELF);

    uint64_t total_steps = platform.run(200000, 500);
    EXPECT_GT(total_steps, 0u);

    uint32_t result = platform.host_bus().read32(0x0002FFFC);
    EXPECT_EQ(result, 0xCAFECAFE);
    EXPECT_EQ(platform.accelerator().mailbox().read(0, mmio::AccessSize::Word), 2u);
}

TEST(CoprocessorAccelPlatformTest, RunUntilDeadlineAndStepProgress) {
    McuConvPlatform platform;
    platform.load_host_firmware(CONV_HOST_ELF);
    platform.load_accel_firmware(CONV_ACCEL_ELF);

    auto res = platform.run_until(100, 500);
    EXPECT_EQ(res.reason, StopReason::Deadline);
    EXPECT_EQ(platform.time(), 100u);
}

TEST(CoprocessorAccelPlatformTest, MultiQuantumSensitivity) {
    for (uint64_t quantum : {1u, 8u, 64u, 500u}) {
        McuConvPlatform platform;
        platform.load_host_firmware(CONV_HOST_ELF);
        platform.load_accel_firmware(CONV_ACCEL_ELF);

        uint64_t total_steps = platform.run(200000, quantum);
        EXPECT_GT(total_steps, 0u);

        uint32_t result = platform.host_bus().read32(0x0002FFFC);
        EXPECT_EQ(result, 0xCAFECAFE);
        EXPECT_EQ(platform.accelerator().mailbox().read(0, mmio::AccessSize::Word), 2u);
    }
}

