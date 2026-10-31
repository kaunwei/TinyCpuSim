#include <gtest/gtest.h>
#include "tinyarmsim/platform/virtual_platform.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::platform;

TEST(VirtualPlatformTest, MultiBusIsolation) {
    VirtualPlatform platform;
    auto& host_bus = platform.create_bus("host", 64 * 1024 * 1024);
    auto& sub_bus = platform.create_bus("subsystem", 16 * 1024 * 1024);

    EXPECT_NE(&host_bus, &sub_bus);

    host_bus.write32(0x2000, 0x12345678);
    EXPECT_EQ(host_bus.read32(0x2000), 0x12345678);

    EXPECT_EQ(sub_bus.read32(0x2000), 0);

    sub_bus.write32(0x2000, 0xDEADBEEF);
    EXPECT_EQ(sub_bus.read32(0x2000), 0xDEADBEEF);
    EXPECT_EQ(host_bus.read32(0x2000), 0x12345678);
}

TEST(VirtualPlatformTest, CpuRegistrationAndQuantumExecution) {
    VirtualPlatform platform;
    auto& host_bus = platform.create_bus("host", 64 * 1024 * 1024);
    auto& sub_bus = platform.create_bus("subsystem", 16 * 1024 * 1024);

    auto& host_cpu = platform.create_cpu("host_cpu", host_bus);
    auto& sub_cpu = platform.create_cpu("sub_cpu", sub_bus);

    EXPECT_EQ(platform.get_cpu("host_cpu"), &host_cpu);
    EXPECT_EQ(platform.get_cpu("sub_cpu"), &sub_cpu);
    EXPECT_EQ(platform.get_cpu("nonexistent"), nullptr);

    host_cpu.state.set_pc(0x1000);
    sub_cpu.state.set_pc(0x1000);

    // 0xBF00 = NOP, 0xDF00 = SVC #0
    host_bus.write16(0x1000, 0xBF00);
    host_bus.write16(0x1002, 0xBF00);
    host_bus.write16(0x1004, 0xDF00);

    sub_bus.write16(0x1000, 0xBF00);
    sub_bus.write16(0x1002, 0xBF00);
    sub_bus.write16(0x1004, 0xDF00);

    // Quantum 1: 1 NOP
    bool active = platform.step_quantum(1);
    EXPECT_TRUE(active);
    EXPECT_EQ(host_cpu.state.get_pc(), 0x1002);
    EXPECT_EQ(sub_cpu.state.get_pc(), 0x1002);

    // Quantum 2: 1 NOP
    active = platform.step_quantum(1);
    EXPECT_TRUE(active);
    EXPECT_EQ(host_cpu.state.get_pc(), 0x1004);
    EXPECT_EQ(sub_cpu.state.get_pc(), 0x1004);

    // Quantum 3: SVC #0 halts
    active = platform.step_quantum(1);
    EXPECT_FALSE(active);
    EXPECT_TRUE(host_cpu.halted);
    EXPECT_TRUE(sub_cpu.halted);
}

TEST(VirtualPlatformTest, RunLoopWithBudget) {
    VirtualPlatform platform;
    auto& host_bus = platform.create_bus("host", 64 * 1024 * 1024);
    auto& host_cpu = platform.create_cpu("host_cpu", host_bus);

    host_cpu.state.set_pc(0x1000);
    for (uint32_t i = 0; i < 5; ++i) {
        host_bus.write16(0x1000 + i * 2, 0xBF00);
    }
    host_bus.write16(0x100A, 0xDF00);

    uint64_t steps = platform.run(1000, 2);
    EXPECT_TRUE(host_cpu.halted);
    EXPECT_GT(steps, 0u);
}
