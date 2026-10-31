#include <gtest/gtest.h>
#include "platforms/02_basic_mcu/topology.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::platform;

TEST(BasicMcuPlatformTest, BootAndExecuteFirmware) {
    BasicMcuPlatform mcu;
    mcu.load_firmware(MCU_FIRMWARE_ELF, 0x00000000);
    uint64_t steps = mcu.run(10000, 500);
    EXPECT_GT(steps, 0u);
    EXPECT_FALSE(mcu.cpu().halted);
}
