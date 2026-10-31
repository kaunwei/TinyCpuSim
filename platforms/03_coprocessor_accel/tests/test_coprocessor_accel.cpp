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
