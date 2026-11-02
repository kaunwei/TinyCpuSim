#include <memory>
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
    EXPECT_FALSE(accel.accel_nvic().is_pending(4));
    host_bus.write32(0x50000014, 1);
    EXPECT_TRUE(accel.accel_nvic().is_pending(4));

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

namespace {
class AcceleratorTickProbe final : public mmio::MMIODevice {
public:
    uint32_t base_address() const noexcept override { return 0x40000000; }
    uint32_t address_span() const noexcept override { return 4; }
    uint32_t read(uint32_t, mmio::AccessSize) override { return 0; }
    void write(uint32_t, uint32_t, mmio::AccessSize) override {}
    void tick(uint64_t cycles) override { ticks += cycles; }
    uint64_t ticks = 0;
};
} // namespace

TEST(AcceleratorSubsystemTest, ParticipantExecutesInstruction) {
    ConvolutionAcceleratorSubsystem accel(4096);
    accel.accel_bus().write16(0x100, 0x202a);
    accel.accel_state().set_pc(0x100);
    const auto result = accel.step();
    EXPECT_EQ(result, StepResult::Progress);
    EXPECT_EQ(accel.accel_state().get_reg(0), 42);
}

TEST(AcceleratorSubsystemTest, ExternalClockSuspendsWfiAndDoesNotTickBus) {
    ConvolutionAcceleratorSubsystem accel(4096);
    auto probe = std::make_unique<AcceleratorTickProbe>();
    auto* ticks = probe.get();
    accel.accel_bus().register_device(std::move(probe));
    accel.accel_bus().write16(0x100, 0xbf30);
    accel.accel_bus().write16(0x102, 0x202a);
    accel.accel_state().set_pc(0x100);
    accel.set_external_clock(true);
    TimeScheduler scheduler;
    const auto result = scheduler.run(10, 8, [&] { return accel.step(); });
    EXPECT_EQ(result.reason, StopReason::Wait);
    EXPECT_EQ(result.progress, 1);
    EXPECT_TRUE(accel.waiting());
    EXPECT_EQ(ticks->ticks, 0);
    const auto waiting_progress = accel.step_quantum(5);
    EXPECT_EQ(waiting_progress, 0);
    EXPECT_EQ(ticks->ticks, 0);
    accel.set_external_clock(false);
    EXPECT_FALSE(accel.waiting());
    const auto resumed_progress = accel.step_quantum(1);
    EXPECT_EQ(resumed_progress, 1);
    EXPECT_EQ(accel.accel_state().get_reg(0), 42);
    EXPECT_EQ(ticks->ticks, 1);
}

TEST(AcceleratorSubsystemTest, SvcHaltsAndStopsQuantumEarly) {
    ConvolutionAcceleratorSubsystem accel(4096);
    accel.accel_bus().write16(0x100, 0x202a);
    accel.accel_bus().write16(0x102, 0xdf00);
    accel.accel_state().set_pc(0x100);
    const auto progress = accel.step_quantum(10);
    EXPECT_EQ(progress, 1);
    EXPECT_TRUE(accel.is_halted());
    const auto halted_result = accel.step();
    EXPECT_EQ(halted_result, StepResult::Halt);
    accel.set_halted(false);
    accel.accel_state().set_pc(0x102);
    const auto svc_result = accel.step();
    EXPECT_EQ(svc_result, StepResult::Halt);
}

TEST(AcceleratorSubsystemTest, FaultLatchesHalt) {
    ConvolutionAcceleratorSubsystem accel(4096);
    accel.accel_state().set_pc(4096);
    const auto result = accel.step();
    EXPECT_EQ(result, StepResult::Fault);
    EXPECT_TRUE(accel.is_halted());
    const auto progress = accel.step_quantum(4);
    EXPECT_EQ(progress, 0);
}

TEST(AcceleratorSubsystemTest, AlreadyHaltedParticipantMakesNoProgress) {
    ConvolutionAcceleratorSubsystem accel(4096);
    accel.set_halted(true);
    const auto result = accel.step();
    EXPECT_EQ(result, StepResult::Halt);
    const auto progress = accel.step_quantum(10);
    EXPECT_EQ(progress, 0);
}
