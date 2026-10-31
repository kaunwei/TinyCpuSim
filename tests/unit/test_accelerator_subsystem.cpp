#include <gtest/gtest.h>
#include "tinyarmsim/platform/accelerator_subsystem.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::platform;

TEST(AcceleratorSubsystemTest, AttachAndMailboxMapping) {
    MemoryBus host_bus(64 * 1024 * 1024);
    ConvolutionAcceleratorSubsystem accel;

    accel.attach_to_host_bus(host_bus);

    // Host writes status / param registers via host_bus
    host_bus.write32(0x50000000, 0x1);
    EXPECT_EQ(host_bus.read32(0x50000000), 0x1);
    EXPECT_EQ(accel.mailbox().read(0, mmio::AccessSize::Word), 0x1);

    // Host writes scratchpad RAM
    host_bus.write32(0x50001000, 0xABCD1234);
    EXPECT_EQ(host_bus.read32(0x50001000), 0xABCD1234);
    // Accel bus also sees scratchpad RAM
    EXPECT_EQ(accel.accel_bus().read32(0x50001000), 0xABCD1234);
}

TEST(AcceleratorSubsystemTest, BidirectionalDoorbellInterrupt) {
    MemoryBus host_bus(64 * 1024 * 1024);
    ConvolutionAcceleratorSubsystem accel;
    accel.attach_to_host_bus(host_bus);

    // Host rings DOORBELL_TO_ACCEL (0x50000014)
    EXPECT_FALSE(accel.accel_nvic().is_pending(16));
    host_bus.write32(0x50000014, 1);
    EXPECT_TRUE(accel.accel_nvic().is_pending(16));

    // Accelerator rings DOORBELL_TO_HOST (0x50000018) via private bus
    EXPECT_FALSE(accel.mailbox().has_pending_irq());
    accel.accel_bus().write32(0x50000018, 1);
    EXPECT_TRUE(accel.mailbox().has_pending_irq());
}

TEST(AcceleratorSubsystemTest, AddressSpaceIsolation) {
    MemoryBus host_bus(64 * 1024 * 1024);
    ConvolutionAcceleratorSubsystem accel;
    accel.attach_to_host_bus(host_bus);

    // Write internal accelerator private RAM at 0x00020000 (128KB, within 16MB)
    accel.accel_bus().write32(0x00020000, 0xDEADBEEF);
    EXPECT_EQ(accel.accel_bus().read32(0x00020000), 0xDEADBEEF);

    // Host bus at 0x00020000 is isolated and untouched (reads 0)
    EXPECT_EQ(host_bus.read32(0x00020000), 0);
}
