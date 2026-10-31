#include <gtest/gtest.h>
#include "tinyarmsim/platform/time_scheduler.hpp"

TEST(PlatformTime, RunsToDeadlineWithActualProgress) {
    tinyarmsim::platform::TimeScheduler scheduler;
    unsigned steps = 0;
    auto result = scheduler.run(3, 8, [&] { ++steps; return tinyarmsim::platform::StepResult::Progress; });
    EXPECT_EQ(steps, 3u);
    EXPECT_EQ(result.progress, 3u);
    EXPECT_EQ(result.reason, tinyarmsim::platform::StopReason::Deadline);
}

TEST(PlatformTime, OrdersEventsBeforeResumingExecution) {
    using namespace tinyarmsim::platform;
    TimeScheduler scheduler;
    std::vector<int> order;
    scheduler.schedule(1, EventPhase::Irq, [&] { order.push_back  (2); });
    scheduler.schedule(1, EventPhase::Device, [&] { order.push_back(1); });
    scheduler.schedule(1, EventPhase::Irq, [&] { order.push_back(3); });
    auto result = scheduler.run(2, 1, [&] { order.push_back(0); return StepResult::Progress; });
    EXPECT_EQ(result.progress, 2u);
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 0}));
    EXPECT_THROW(scheduler.schedule(1, EventPhase::Device, [] {}), std::invalid_argument);
}

TEST(PlatformTime, ClockRetainsRemainderAndChecksOverflow) {
    tinyarmsim::platform::ClockConversion clock(2, 3);
    EXPECT_EQ(clock.convert(1), 0u);
    EXPECT_EQ(clock.convert(1), 1u);
    EXPECT_EQ(clock.convert(1), 1u);
    EXPECT_THROW(tinyarmsim::platform::ClockConversion(1, 0), std::invalid_argument);
    tinyarmsim::platform::ClockConversion huge(UINT64_MAX, 1);
    EXPECT_THROW(huge.convert(2), std::overflow_error);
}

#include "tinyarmsim/platform/virtual_platform.hpp"

namespace {
class PassiveRegister : public tinyarmsim::mmio::MMIODevice {
public:
    uint32_t value = 0;
    uint32_t base_address() const noexcept override { return 0x40000000; }
    uint32_t address_span() const noexcept override { return 4; }
    uint32_t read(uint32_t, tinyarmsim::mmio::AccessSize) override { return value; }
    void write(uint32_t, uint32_t v, tinyarmsim::mmio::AccessSize) override { value = v; }
};
}
TEST(PlatformTime, InterpreterWritesPassiveRegisterAndReceivesScheduledIrq) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    MemoryBus bus(65536);
    CpuInstance cpu("cpu", bus);
    auto device = std::make_unique<PassiveRegister>();
    auto* reg = device.get();
    bus.register_device(std::move(device));
    bus.write16(0x100, 0x6008); // STR r0, [r1]
    bus.write16(0x102, 0x46c0); // NOP
    bus.write32(0x1044, 0x2001);
    bus.write16(0x2000, 0x2463); // MOVS r4, #99
    cpu.state.set_pc(0x100);
    cpu.state.set_reg(13, 0x8000);
    cpu.state.set_reg(0, 42);
    cpu.state.set_reg(1, 0x40000000);
    cpu.nvic.set_vtor(0x1000);
    cpu.nvic.write(0x100, 2, mmio::AccessSize::Word);
    TimeScheduler scheduler;
    scheduler.schedule(1, EventPhase::Irq, [&] { cpu.nvic.set_pending(1); });
    const auto result = scheduler.run_interpreter(3, 8, cpu.interpreter);
    EXPECT_EQ(result.progress, 3u);
    EXPECT_EQ(reg->value, 42u);
    EXPECT_EQ(cpu.state.get_reg(4), 99u);
}

TEST(PlatformTime, RejectsInvalidRunsAndReportsStops) {
    using namespace tinyarmsim::platform;
    TimeScheduler scheduler;
    EXPECT_THROW(scheduler.run(1, 0, [] { return StepResult::Progress; }), std::invalid_argument);
    unsigned count = 0;
    const auto halt = scheduler.run(5, 8, [&] { return ++count == 2 ? StepResult::Halt : StepResult::Progress; });
    EXPECT_EQ(halt.progress, 1u);
    EXPECT_EQ(halt.reason, StopReason::Halt);
    EXPECT_EQ(scheduler.now(), 1u);
    const auto fault = scheduler.run(5, 1, [] { return StepResult::Fault; });
    EXPECT_EQ(fault.progress, 0u);
    EXPECT_EQ(fault.reason, StopReason::Fault);
    EXPECT_THROW(scheduler.run(0, 1, [] { return StepResult::Progress; }), std::invalid_argument);
}

TEST(PlatformTime, BoundsSameTimeChains) {
    using namespace tinyarmsim::platform;
    TimeScheduler scheduler;
    std::function<void()> callback;
    callback = [&] { scheduler.schedule(0, EventPhase::Device, callback); };
    scheduler.schedule(0, EventPhase::Device, callback);
    EXPECT_THROW(scheduler.run(1, 1, [] { return StepResult::Progress; }), std::runtime_error);
}

TEST(PlatformTime, DoesNotReopenAnEarlierPhaseAtTheSameTime) {
    using namespace tinyarmsim::platform;
    TimeScheduler scheduler;
    scheduler.schedule(0, EventPhase::Irq, [] {});
    scheduler.run(0, 1, [] { return StepResult::Progress; });
    EXPECT_THROW(scheduler.schedule(0, EventPhase::Device, [] {}), std::invalid_argument);
}

TEST(PlatformTime, DispatchesStepScheduledEventsBeforeAdvancingTime) {
    using namespace tinyarmsim::platform;
    for (const uint64_t quantum : {1u, 8u}) {
        TimeScheduler scheduler;
        std::vector<int> order;
        scheduler.schedule(1, EventPhase::Device, [&] { order.push_back(2); });
        const auto result = scheduler.run(2, quantum, [&] {
            order.push_back(0);
            if (scheduler.now() == 0)
                scheduler.schedule(0, EventPhase::Device, [&] { order.push_back(1); });
            return StepResult::Progress;
        });
        EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 0}));
        EXPECT_EQ(result.progress, 2u);
        EXPECT_EQ(result.reason, StopReason::Deadline);
        EXPECT_EQ(scheduler.now(), 2u);
    }
}

TEST(PlatformTime, PreservesStepEventsOnStopsAndExceptions) {
    using namespace tinyarmsim::platform;
    for (const auto stop : {StepResult::Halt, StepResult::Fault}) {
        TimeScheduler scheduler;
        unsigned callbacks = 0;
        const auto result = scheduler.run(1, 1, [&] {
            scheduler.schedule(scheduler.now(), EventPhase::Device, [&] { ++callbacks; });
            return stop;
        });
        EXPECT_EQ(result.progress, 0u);
        EXPECT_EQ(result.reason, stop == StepResult::Halt ? StopReason::Halt : StopReason::Fault);
        EXPECT_EQ(scheduler.now(), 0u);
        EXPECT_EQ(callbacks, 0u);
        scheduler.run(0, 8, [] { return StepResult::Progress; });
        EXPECT_EQ(callbacks, 1u);
    }
    TimeScheduler scheduler;
    unsigned callbacks = 0;
    EXPECT_THROW(scheduler.run(1, 1, [&]() -> StepResult {
        scheduler.schedule(0, EventPhase::Device, [&] { ++callbacks; });
        throw std::runtime_error("step failed");
    }), std::runtime_error);
    EXPECT_EQ(scheduler.now(), 0u);
    scheduler.run(0, 1, [] { return StepResult::Progress; });
    EXPECT_EQ(callbacks, 1u);
}

TEST(PlatformTime, PreservesRemainingEventsWhenStepScheduledCallbackThrows) {
    using namespace tinyarmsim::platform;
    TimeScheduler scheduler;
    std::vector<int> order;
    EXPECT_THROW(scheduler.run(1, 8, [&] {
        scheduler.schedule(0, EventPhase::Device, [&] { order.push_back(1); throw std::runtime_error("callback failed"); });
        scheduler.schedule(0, EventPhase::Irq, [&] { order.push_back(2); });
        return StepResult::Progress;
    }), std::runtime_error);
    EXPECT_EQ(scheduler.now(), 0u);
    const auto result = scheduler.run(0, 1, [] { return StepResult::Progress; });
    EXPECT_EQ(result.progress, 0u);
    EXPECT_EQ(result.reason, StopReason::Deadline);
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
    EXPECT_THROW(scheduler.schedule(0, EventPhase::Device, [] {}), std::invalid_argument);
}
