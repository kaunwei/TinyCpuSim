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

#include "tinyarmsim/platform/timer_adapter.hpp"
#include "tinyarmsim/platform/proxy_device.hpp"

TEST(PlatformTime, RealTimerAdvancesOnceAndPublishesIrqAcrossQuanta) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    for (uint64_t quantum : {1u, 8u, 64u, 500u}) {
        MemoryBus bus(65536);
        CpuInstance cpu("cpu", bus);
        auto timer = std::make_unique<mmio::TimerDevice>();
        auto* device = timer.get();
        bus.register_device(std::move(timer));
        bus.register_device(std::make_unique<ProxyMMIODevice>(*device, 0x40002000, 0x1000));
        for (uint32_t pc = 0x100; pc < 0x120; pc += 2) bus.write16(pc, 0x46c0);
        bus.write32(0x1044, 0x2001);
        bus.write16(0x2000, 0x2463);
        cpu.state.set_pc(0x100);
        cpu.state.set_reg(13, 0x8000);
        cpu.nvic.set_vtor(0x1000);
        cpu.nvic.write(0x100, 2, mmio::AccessSize::Word);
        bus.write32(0x40002000, 3);
        bus.write32(0x40001008, 5);
        cpu.interpreter.set_external_clock(true);
        TimeScheduler scheduler;
        TimerAdapter adapter(scheduler, *device, cpu.nvic);
        EXPECT_EQ(scheduler.run_interpreter(2, quantum, cpu.interpreter).progress, 2u);
        EXPECT_EQ(bus.read32(0x40002004), 1u);
        EXPECT_EQ(bus.read32(0x40001010), 0u);
        EXPECT_EQ(scheduler.run_interpreter(5, quantum, cpu.interpreter).progress, 3u);
        EXPECT_EQ(bus.read32(0x40001004), 0u);
        EXPECT_EQ(bus.read32(0x40002014), 1u);
        EXPECT_EQ(cpu.state.get_reg(4), 99u);
    }
}

TEST(PlatformTime, CallbackFailureCannotReplayCommittedStep) {
    using namespace tinyarmsim::platform;
    TimeScheduler scheduler;
    unsigned steps = 0;
    EXPECT_THROW(scheduler.run(1, 1, [&] {
        ++steps;
        scheduler.schedule(0, EventPhase::Device, [] { throw std::runtime_error("failed"); });
        return StepResult::Progress;
    }), std::runtime_error);
    EXPECT_THROW(scheduler.run(1, 1, [&] { ++steps; return StepResult::Progress; }), std::runtime_error);
    EXPECT_EQ(steps, 1u);
}

TEST(PlatformTime, ExternalClockCoversSkippedItAndExceptionEntry) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    MemoryBus bus(65536);
    CpuInstance cpu("cpu", bus);
    auto timer = std::make_unique<mmio::TimerDevice>();
    auto* device = timer.get();
    bus.register_device(std::move(timer));
    bus.write16(0x100, 0x2001);
    bus.write16(0x102, 0x46c0);
    bus.write32(0x1044, 0x2001);
    cpu.state.set_pc(0x100);
    cpu.state.set_reg(13, 0x8000);
    cpu.state.set_it_state(0x08); // EQ, false with reset flags
    cpu.nvic.set_vtor(0x1000);
    cpu.nvic.write(0x100, 2, mmio::AccessSize::Word);
    bus.write32(0x40001000, 10);
    bus.write32(0x40001008, 1);
    cpu.interpreter.set_external_clock(true);
    TimeScheduler scheduler;
    TimerAdapter adapter(scheduler, *device, cpu.nvic);
    EXPECT_EQ(scheduler.run_interpreter(1, 1, cpu.interpreter).progress, 1u);
    EXPECT_EQ(cpu.state.get_reg(0), 0u);
    EXPECT_EQ(bus.read32(0x40001004), 9u);
    cpu.nvic.set_pending(1);
    EXPECT_EQ(scheduler.run_interpreter(2, 1, cpu.interpreter).progress, 1u);
    EXPECT_EQ(cpu.state.get_pc(), 0x2000u);
    EXPECT_EQ(bus.read32(0x40001004), 8u);
}

TEST(PlatformTime, TimerClockRetainsFractionalTicks) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    MemoryBus bus(65536);
    CpuInstance cpu("cpu", bus);
    auto timer = std::make_unique<mmio::TimerDevice>();
    auto* device = timer.get();
    bus.register_device(std::move(timer));
    for (uint32_t pc = 0x100; pc < 0x110; pc += 2) bus.write16(pc, 0x46c0);
    cpu.state.set_pc(0x100);
    bus.write32(0x40001000, 10);
    bus.write32(0x40001008, 1);
    cpu.interpreter.set_external_clock(true);
    TimeScheduler scheduler;
    TimerAdapter adapter(scheduler, *device, cpu.nvic, 2, 3);
    EXPECT_EQ(scheduler.run_interpreter(2, 64, cpu.interpreter).progress, 2u);
    EXPECT_EQ(bus.read32(0x40001004), 9u);
    EXPECT_EQ(scheduler.run_interpreter(3, 500, cpu.interpreter).progress, 1u);
    EXPECT_EQ(bus.read32(0x40001004), 8u);
}

TEST(PlatformTime, ExternalClockWfiWaitsWithoutEvents) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    MemoryBus bus(65536);
    CpuInstance cpu("cpu", bus);
    bus.write16(0x100, 0xbf30); // WFI
    bus.write16(0x102, 0x202a); // MOVS r0, #42
    cpu.state.set_pc(0x100);
    cpu.interpreter.set_external_clock(true);
    TimeScheduler scheduler;
    const auto result = scheduler.run_interpreter(10, 8, cpu.interpreter);
    EXPECT_EQ(result.reason, StopReason::Wait);
    EXPECT_EQ(result.progress, 1u);
    EXPECT_EQ(scheduler.now(), 1u);
    EXPECT_EQ(cpu.state.get_pc(), 0x102u);
    EXPECT_EQ(cpu.state.get_reg(0), 0u);
}

TEST(PlatformTime, WfiRealTimerWakesAcrossQuanta) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    for (uint64_t quantum : {1u, 8u, 64u, 500u}) {
        MemoryBus bus(65536);
        CpuInstance cpu("cpu", bus);
        auto timer = std::make_unique<mmio::TimerDevice>();
        auto* device = timer.get();
        bus.register_device(std::move(timer));
        bus.write16(0x100, 0xbf30);
        bus.write16(0x102, 0x202a);
        bus.write32(0x1044, 0x2001);
        bus.write16(0x2000, 0x2463);
        cpu.state.set_pc(0x100);
        cpu.state.set_reg(13, 0x8000);
        cpu.nvic.set_vtor(0x1000);
        cpu.nvic.write(0x100, 2, mmio::AccessSize::Word);
        bus.write32(0x40001000, 3);
        bus.write32(0x40001008, 5);
        cpu.interpreter.set_external_clock(true);
        TimeScheduler scheduler;
        TimerAdapter adapter(scheduler, *device, cpu.nvic);
        const auto before = scheduler.run_interpreter(3, quantum, cpu.interpreter);
        EXPECT_EQ(before.reason, StopReason::Deadline);
        EXPECT_EQ(before.progress, 1u);
        EXPECT_EQ(cpu.state.get_pc(), 0x102u);
        EXPECT_TRUE(cpu.nvic.is_pending(1));
        const auto after = scheduler.run_interpreter(5, quantum, cpu.interpreter);
        EXPECT_EQ(after.progress, 2u);
        EXPECT_EQ(cpu.state.get_reg(4), 99u);
        EXPECT_EQ(cpu.state.get_reg(0), 0u);
    }
}

TEST(PlatformTime, WfiPendingAndPrimaskDoNotLoseWakeup) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    for (uint64_t quantum : {1u, 8u, 64u, 500u}) {
        MemoryBus bus(65536);
        CpuInstance cpu("cpu", bus);
        bus.write16(0x100, 0xbf30);
        bus.write16(0x102, 0x202a);
        cpu.state.set_pc(0x100);
        cpu.state.set_primask(true);
        cpu.nvic.write(0x100, 2, mmio::AccessSize::Word);
        cpu.nvic.set_pending(1);
        cpu.interpreter.set_external_clock(true);
        TimeScheduler scheduler;
        const auto result = scheduler.run_interpreter(2, quantum, cpu.interpreter);
        EXPECT_EQ(result.progress, 2u);
        EXPECT_EQ(cpu.state.get_reg(0), 42u);
        EXPECT_TRUE(cpu.nvic.is_pending(1));
    }
}

TEST(PlatformTime, DisabledPendingIrqDoesNotWakeAndFutureEventIsRetained) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    MemoryBus bus(65536);
    CpuInstance cpu("cpu", bus);
    bus.write16(0x100, 0xbf30);
    cpu.state.set_pc(0x100);
    cpu.nvic.set_pending(1);
    cpu.interpreter.set_external_clock(true);
    TimeScheduler scheduler;
    scheduler.schedule(5, EventPhase::Irq, [&] { cpu.nvic.write(0x100, 2, mmio::AccessSize::Word); });
    const auto result = scheduler.run_interpreter(4, 8, cpu.interpreter);
    EXPECT_EQ(result.reason, StopReason::Wait);
    EXPECT_EQ(result.progress, 1u);
    EXPECT_EQ(scheduler.now(), 1u);
    EXPECT_TRUE(cpu.interpreter.waiting());
}

TEST(PlatformTime, WfiRespectsActiveExceptionPriority) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    MemoryBus bus(65536);
    CpuInstance cpu("cpu", bus);
    bus.write16(0x100, 0xbf30);
    cpu.state.set_pc(0x100);
    cpu.nvic.write(0x100, 3, mmio::AccessSize::Word);
    cpu.nvic.acknowledge_irq(0);
    cpu.nvic.set_pending(1);
    cpu.interpreter.set_external_clock(true);
    TimeScheduler scheduler;
    const auto result = scheduler.run_interpreter(3, 1, cpu.interpreter);
    EXPECT_EQ(result.reason, StopReason::Wait);
    EXPECT_EQ(result.progress, 1u);
    EXPECT_EQ(cpu.state.get_pc(), 0x102u);
}

TEST(PlatformTime, CancelledLastEventDoesNotKeepWaitAlive) {
    using namespace tinyarmsim::platform;
    TimeScheduler scheduler;
    bool called = false;
    auto event = scheduler.schedule(3, EventPhase::Device, [&] { called = true; });
    event.cancel();
    event.cancel();
    const auto result = scheduler.run(10, 1, [] { return StepResult::Wait; });
    EXPECT_EQ(result.reason, StopReason::Wait);
    EXPECT_EQ(scheduler.now(), 0u);
    EXPECT_FALSE(called);
}

TEST(PlatformTime, AdapterDestructionCancelsOwnedEvents) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    TimeScheduler scheduler;
    mmio::TimerDevice timer;
    mmio::NVICDevice nvic;
    { TimerAdapter adapter(scheduler, timer, nvic); }
    const auto result = scheduler.run(10, 1, [] { return StepResult::Wait; });
    EXPECT_EQ(result.reason, StopReason::Wait);
    EXPECT_EQ(scheduler.now(), 0u);
}

TEST(PlatformTime, StopAndResetSuppressSameTimeStaleIrq) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    for (bool reset : {false, true}) {
        TimeScheduler scheduler;
        mmio::TimerDevice timer;
        mmio::NVICDevice nvic;
        timer.write(0, 1, mmio::AccessSize::Word);
        timer.write(8, 5, mmio::AccessSize::Word);
        TimerAdapter adapter(scheduler, timer, nvic);
        scheduler.schedule(1, EventPhase::Device, [&] {
            if (reset) adapter.reset(); else adapter.stop();
        });
        scheduler.run(1, 1, [] { return StepResult::Progress; });
        EXPECT_TRUE(timer.has_pending_irq());
        EXPECT_FALSE(nvic.is_pending(1));
        adapter.stop();
        adapter.stop();
        EXPECT_EQ(scheduler.run(3, 1, [] { return StepResult::Wait; }).reason, StopReason::Wait);
    }
}

TEST(PlatformTime, ResetRestartsFractionalClockFromCurrentTime) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    TimeScheduler scheduler;
    mmio::TimerDevice timer;
    mmio::NVICDevice nvic;
    timer.write(0, 10, mmio::AccessSize::Word);
    timer.write(8, 1, mmio::AccessSize::Word);
    TimerAdapter adapter(scheduler, timer, nvic, 2, 3);
    scheduler.run(1, 1, [] { return StepResult::Progress; });
    adapter.reset();
    adapter.reset();
    scheduler.run(2, 1, [] { return StepResult::Progress; });
    EXPECT_EQ(timer.read(4, mmio::AccessSize::Word), 10u);
    scheduler.run(3, 1, [] { return StepResult::Progress; });
    EXPECT_EQ(timer.read(4, mmio::AccessSize::Word), 9u);
}

TEST(PlatformTime, AdapterCanBeDestroyedAfterScheduler) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    mmio::TimerDevice timer;
    mmio::NVICDevice nvic;
    std::unique_ptr<TimerAdapter> adapter;
    {
        TimeScheduler scheduler;
        adapter = std::make_unique<TimerAdapter>(scheduler, timer, nvic);
    }
    adapter.reset();
}

#include "../platforms/02_basic_mcu/topology.hpp"

TEST(PlatformTime, McuTimerAndIdleWaitUseActualProgress) {
    using namespace tinyarmsim::platform;
    for (uint64_t quantum : {1u, 8u, 64u, 500u}) {
        BasicMcuPlatform mcu;
        auto& bus = mcu.bus();
        bus.write16(0x100, 0x46c0);
        bus.write16(0x102, 0xbf30);
        mcu.cpu().state.set_pc(0x100);
        bus.write32(BasicMcuPlatform::TIMER_BASE, 3);
        bus.write32(BasicMcuPlatform::TIMER_BASE + 8, 1);
        auto result = mcu.run_until(10, quantum);
        EXPECT_EQ(result.reason, StopReason::Wait);
        EXPECT_EQ(result.progress, 2u);
        EXPECT_EQ(mcu.time(), 3u);
        EXPECT_EQ(bus.read32(BasicMcuPlatform::TIMER_BASE + 4), 0u);
        EXPECT_EQ(bus.read32(BasicMcuPlatform::TIMER_BASE + 0x10), 1u);
    }
}

TEST(PlatformTime, McuTimerWakesWfiAndAliasesAdvanceOnce) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    for (uint64_t quantum : {1u, 8u, 64u, 500u}) {
        BasicMcuPlatform mcu;
        auto& bus = mcu.bus();
        auto* timer = bus.get_device_at<mmio::TimerDevice>(BasicMcuPlatform::TIMER_BASE);
        bus.register_device(std::make_unique<ProxyMMIODevice>(*timer, 0x40003000, 0x1000));
        bus.write16(0x100, 0xbf30);
        bus.write16(0x102, 0x202a);
        mcu.cpu().state.set_pc(0x100);
        mcu.cpu().state.set_primask(true);
        mcu.cpu().nvic.write(0x100, 2, mmio::AccessSize::Word);
        bus.write32(0x40003000, 3);
        bus.write32(BasicMcuPlatform::TIMER_BASE + 8, 5);
        auto before = mcu.run_until(2, quantum);
        EXPECT_EQ(before.progress, 1u);
        EXPECT_EQ(bus.read32(0x40003004), 1u);
        auto after = mcu.run_until(4, quantum);
        EXPECT_EQ(after.progress, 1u);
        EXPECT_EQ(mcu.cpu().state.get_reg(0), 42u);
        EXPECT_TRUE(mcu.cpu().nvic.is_pending(1));
    }
}

TEST(PlatformTime, McuClockedDevicesProgressDuringWait) {
    using namespace tinyarmsim::platform;
    BasicMcuPlatform mcu;
    auto& bus = mcu.bus();
    bus.write16(0x100, 0xbf30);
    mcu.cpu().state.set_pc(0x100);
    bus.write32(BasicMcuPlatform::UART_BASE + 0x10, 1);
    bus.write32(BasicMcuPlatform::UART_BASE, 'A');
    bus.write32(BasicMcuPlatform::GPIO_BASE + 0x18, 3);
    auto* gpio = bus.get_device_at<tinyarmsim::mmio::GpioDevice>(BasicMcuPlatform::GPIO_BASE);
    gpio->inject_external_input(1);
    auto result = mcu.run_until(20, 8);
    EXPECT_EQ(result.reason, StopReason::Wait);
    EXPECT_EQ(result.progress, 1u);
    EXPECT_EQ(mcu.time(), 10u);
    EXPECT_EQ(bus.read32(BasicMcuPlatform::GPIO_BASE + 4), 1u);
    EXPECT_EQ(bus.get_device_at<tinyarmsim::mmio::UartDevice>(BasicMcuPlatform::UART_BASE)->get_tx_output(), "A");
}

TEST(PlatformTime, McuUartAsymmetricClockDuringWait) {
    using namespace tinyarmsim::platform;
    for (uint64_t quantum : {1u, 8u, 64u, 500u}) {
        BasicMcuPlatform mcu(2, 3);
        auto& bus = mcu.bus();
        bus.write16(0x100, 0xbf30);
        mcu.cpu().state.set_pc(0x100);
        bus.write32(BasicMcuPlatform::UART_BASE + 0x10, 1);
        bus.write32(BasicMcuPlatform::UART_BASE, 'A');
        auto before = mcu.run_until(14, quantum);
        EXPECT_EQ(before.progress, 1u);
        EXPECT_EQ(bus.read32(BasicMcuPlatform::UART_BASE + 4) & 8u, 8u);
        auto at = mcu.run_until(15, quantum);
        EXPECT_EQ(at.progress, 0u);
        EXPECT_EQ(mcu.time(), 15u);
        EXPECT_EQ(bus.get_device_at<tinyarmsim::mmio::UartDevice>(BasicMcuPlatform::UART_BASE)->get_tx_output(), "A");
    }
}

TEST(PlatformTime, McuUartRemainderResetAndRxIrq) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    for (uint64_t quantum : {1u, 8u, 64u, 500u}) {
        BasicMcuPlatform mcu(4, 3);
        auto& bus = mcu.bus();
        auto* uart = bus.get_device_at<mmio::UartDevice>(BasicMcuPlatform::UART_BASE);
        bus.write16(0x100, 0xbf30);
        mcu.cpu().state.set_pc(0x100);
        bus.write32(BasicMcuPlatform::UART_BASE + 0x10, 1);
        bus.write32(BasicMcuPlatform::UART_BASE, 'A');
        EXPECT_EQ(mcu.run_until(8, quantum).progress, 1u);
        EXPECT_EQ(uart->get_tx_output(), "A");
        bus.write32(BasicMcuPlatform::UART_BASE, 'B');
        mcu.run_until(14, quantum);
        EXPECT_EQ(uart->get_tx_output(), "A");
        mcu.run_until(15, quantum);
        EXPECT_EQ(uart->get_tx_output(), "AB");

        bus.write32(BasicMcuPlatform::UART_BASE, 'X');
        mcu.run_until(16, quantum);
        uart->reset();
        bus.write32(BasicMcuPlatform::UART_BASE + 0x10, 1);
        bus.write32(BasicMcuPlatform::UART_BASE + 8, 7);
        uart->set_tx_consumer([uart](uint8_t byte) { uart->inject_rx(byte); });
        bus.write32(BasicMcuPlatform::UART_BASE, 'C');
        mcu.run_until(23, quantum);
        EXPECT_FALSE(mcu.cpu().nvic.is_pending(0));
        EXPECT_EQ(uart->rx_size(), 0u);
        mcu.run_until(24, quantum);
        EXPECT_TRUE(mcu.cpu().nvic.is_pending(0));
        EXPECT_EQ(bus.read32(BasicMcuPlatform::UART_BASE), 'C');
        EXPECT_EQ(uart->get_tx_output(), "");
    }
}

TEST(PlatformTime, McuPendingMaskedIrqAndRelativeRun) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    for (uint64_t quantum : {1u, 8u, 64u, 500u}) {
        BasicMcuPlatform mcu;
        mcu.bus().write16(0x100, 0xbf30);
        mcu.bus().write16(0x102, 0x202a);
        mcu.cpu().state.set_pc(0x100);
        mcu.cpu().state.set_primask(true);
        mcu.cpu().nvic.write(0x100, 2, mmio::AccessSize::Word);
        mcu.cpu().nvic.set_pending(1);
        EXPECT_EQ(mcu.run(2, quantum), 2u);
        EXPECT_EQ(mcu.time(), 2u);
        EXPECT_EQ(mcu.cpu().state.get_reg(0), 42u);
        EXPECT_TRUE(mcu.cpu().nvic.is_pending(1));
    }
}

TEST(PlatformTime, McuTimerInterruptsExecutingCpu) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    for (uint64_t quantum : {1u, 8u, 64u, 500u}) {
        BasicMcuPlatform mcu;
        auto& bus = mcu.bus();
        for (uint32_t pc = 0x100; pc < 0x110; pc += 2) bus.write16(pc, 0x46c0);
        bus.write32(0x1044, 0x2001);
        bus.write16(0x2000, 0x2463);
        mcu.cpu().state.set_pc(0x100);
        mcu.cpu().state.set_reg(13, 0x8000);
        mcu.cpu().nvic.set_vtor(0x1000);
        mcu.cpu().nvic.write(0x100, 2, mmio::AccessSize::Word);
        bus.write32(BasicMcuPlatform::TIMER_BASE, 3);
        bus.write32(BasicMcuPlatform::TIMER_BASE + 8, 5);
        EXPECT_EQ(mcu.run_until(5, quantum).progress, 5u);
        EXPECT_EQ(mcu.cpu().state.get_reg(4), 99u);
        EXPECT_EQ(bus.read32(BasicMcuPlatform::TIMER_BASE + 0x10), 1u);
    }
}

TEST(PlatformTime, UartResetBetweenDeviceAndIrqRejectsStaleNotification) {
    using namespace tinyarmsim;
    using namespace tinyarmsim::platform;
    TimeScheduler scheduler;
    mmio::UartDevice uart;
    mmio::NVICDevice nvic;
    uart.write(0x10, 1, mmio::AccessSize::Word);
    uart.write(0, 'A', mmio::AccessSize::Word);
    UartAdapter adapter(scheduler, uart, nvic);
    adapter.refresh();
    scheduler.schedule(1, EventPhase::Device, [&] {
        uart.reset();
        uart.write(8, 7, mmio::AccessSize::Word);
        uart.inject_rx('R');
    });
    scheduler.run(1, 1, [] { return StepResult::Progress; });
    EXPECT_TRUE(uart.has_pending_irq());
    EXPECT_FALSE(nvic.is_pending(uart.irq_line()));

    uart.write(0x10, 1, mmio::AccessSize::Word);
    uart.write(0, 'B', mmio::AccessSize::Word);
    adapter.refresh();
    scheduler.run(2, 1, [] { return StepResult::Progress; });
    EXPECT_TRUE(nvic.is_pending(uart.irq_line()));
    EXPECT_EQ(uart.read(0, mmio::AccessSize::Word), 'R');
}
