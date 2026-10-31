#include <gtest/gtest.h>
#include "platforms/mcu_conv_platform.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::platform;

TEST(VirtualPlatformIntegrationTest, HostAcceleratorConvolutionCoSimulation) {
    McuConvPlatform platform;

    // Load ELFs into Host and Accelerator
    platform.load_host_firmware(CONV_HOST_ELF);
    platform.load_accel_firmware(CONV_ACCEL_ELF);

    // Co-simulate up to 200,000 instructions
    uint64_t total_steps = platform.run(200000, 500);
    EXPECT_GT(total_steps, 0u);

    // Verify Host completed successfully (wrote 0xCAFECAFE to test result address 0x0002FFFC)
    uint32_t result = platform.host_bus().read32(0x0002FFFC);
    EXPECT_EQ(result, 0xCAFECAFE);

    // Verify Accelerator status is DONE (MB_STATUS_DONE = 2)
    EXPECT_EQ(platform.accelerator().mailbox().read(0, mmio::AccessSize::Word), 2u);
}
