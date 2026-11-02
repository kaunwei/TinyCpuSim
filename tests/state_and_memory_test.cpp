#include <gtest/gtest.h>
#include "tinyarmsim/cpu/faults.hpp"
#include "tinyarmsim/cpu/state.hpp"
#include "tinyarmsim/memory/memory_bus.hpp"

using namespace tinyarmsim;

// === ArchitecturalState Tests ===

TEST(ArchitecturalStateTest, InitialStateIsZeroed) {
    ArchitecturalState state;
    for (size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(state.get_reg(i), 0u);
    }
    EXPECT_EQ(state.get_cpsr(), 0u);
    EXPECT_FALSE(state.get_flag_n());
    EXPECT_FALSE(state.get_flag_z());
    EXPECT_FALSE(state.get_flag_c());
    EXPECT_FALSE(state.get_flag_v());
}

TEST(ArchitecturalStateTest, RegisterReadWriteAndAliases) {
    ArchitecturalState state;
    state.set_reg(0, 0x12345678);
    state.set_reg(13, 0x20000000); // SP
    state.set_reg(14, 0x08000100); // LR
    state.set_reg(15, 0x08000200); // PC

    EXPECT_EQ(state.get_reg(0), 0x12345678u);
    EXPECT_EQ(state.get_sp(), 0x20000000u);
    EXPECT_EQ(state.get_lr(), 0x08000100u);
    EXPECT_EQ(state.get_pc(), 0x08000200u);

    state.advance_pc(4);
    EXPECT_EQ(state.get_pc(), 0x08000204u);
    state.advance_pc(2);
    EXPECT_EQ(state.get_pc(), 0x08000206u);
}

TEST(ArchitecturalStateTest, ConditionFlagsMutation) {
    ArchitecturalState state;
    state.set_flags(true, false, true, false);
    EXPECT_TRUE(state.get_flag_n());
    EXPECT_FALSE(state.get_flag_z());
    EXPECT_TRUE(state.get_flag_c());
    EXPECT_FALSE(state.get_flag_v());

    state.set_flag_z(true);
    EXPECT_TRUE(state.get_flag_z());
    state.set_flag_n(false);
    EXPECT_FALSE(state.get_flag_n());
}

TEST(ArchitecturalStateTest, RegisterOutOfBoundsThrowsFault) {
    ArchitecturalState state;
    EXPECT_THROW((void)state.get_reg(16), CpuFaultException);
    EXPECT_THROW(state.set_reg(16, 0), CpuFaultException);
}

// === MemoryBus Tests ===

TEST(MemoryBusTest, ReadWriteByteHalfwordWord) {
    MemoryBus bus(64 * 1024 * 1024); // 64MB

    bus.write8(0x1000, 0xAB);
    EXPECT_EQ(bus.read8(0x1000), 0xABu);

    bus.write16(0x2000, 0xCDEF);
    EXPECT_EQ(bus.read16(0x2000), 0xCDEFu);

    bus.write32(0x3000, 0x12345678);
    EXPECT_EQ(bus.read32(0x3000), 0x12345678u);

    // Little-endian verification
    EXPECT_EQ(bus.read8(0x3000), 0x78u);
    EXPECT_EQ(bus.read8(0x3001), 0x56u);
    EXPECT_EQ(bus.read8(0x3002), 0x34u);
    EXPECT_EQ(bus.read8(0x3003), 0x12u);
}

TEST(MemoryBusTest, UnalignedAccessThrowsMemoryFault) {
    MemoryBus bus(64 * 1024 * 1024);

    EXPECT_THROW((void)bus.read16(0x1001), MemoryFaultException);
    EXPECT_THROW(bus.write16(0x1003, 0x1234), MemoryFaultException);

    EXPECT_THROW((void)bus.read32(0x1001), MemoryFaultException);
    EXPECT_THROW((void)bus.read32(0x1002), MemoryFaultException);
    EXPECT_THROW((void)bus.read32(0x1003), MemoryFaultException);
    EXPECT_THROW(bus.write32(0x1002, 0x12345678), MemoryFaultException);
}

TEST(MemoryBusTest, OutOfBoundsAccessThrowsMemoryFault) {
    uint32_t size = 1024 * 1024; // 1MB
    MemoryBus bus(size);

    // Valid access at size - 4
    bus.write32(size - 4, 0xDEADBEEF);
    EXPECT_EQ(bus.read32(size - 4), 0xDEADBEEFu);

    // Out of bounds
    EXPECT_THROW((void)bus.read8(size), MemoryFaultException);
    EXPECT_THROW(bus.write8(size, 0xFF), MemoryFaultException);
    EXPECT_THROW((void)bus.read16(size - 1), MemoryFaultException);
    EXPECT_THROW((void)bus.read32(size - 2), MemoryFaultException);
    EXPECT_THROW((void)bus.read32(size), MemoryFaultException);
}
