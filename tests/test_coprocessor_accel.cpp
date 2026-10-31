#include <cassert>
#include <memory>
#include "tinyarmsim/platform/accelerator_subsystem.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::platform;

class TickProbe final : public mmio::MMIODevice {
public:
    uint32_t base_address() const noexcept override { return 0x40000000; }
    uint32_t address_span() const noexcept override { return 4; }
    uint32_t read(uint32_t, mmio::AccessSize) override { return 0; }
    void write(uint32_t, uint32_t, mmio::AccessSize) override {}
    void tick(uint64_t cycles) override { ticks += cycles; }
    uint64_t ticks = 0;
};

int main() {
    {
        ConvolutionAcceleratorSubsystem accel(4096);
        accel.accel_bus().write16(0x100, 0x202a);
        accel.accel_state().set_pc(0x100);
        assert(accel.step() == StepResult::Progress);
        assert(accel.accel_state().get_reg(0) == 42);
    }
    {
        ConvolutionAcceleratorSubsystem accel(4096);
        auto probe = std::make_unique<TickProbe>();
        auto* ticks = probe.get();
        accel.accel_bus().register_device(std::move(probe));
        accel.accel_bus().write16(0x100, 0xbf30);
        accel.accel_bus().write16(0x102, 0x202a);
        accel.accel_state().set_pc(0x100);
        accel.set_external_clock(true);
        TimeScheduler scheduler;
        auto result = scheduler.run(10, 8, [&] { return accel.step(); });
        assert(result.reason == StopReason::Wait && result.progress == 1);
        assert(accel.waiting() && ticks->ticks == 0);
        assert(accel.step_quantum(5) == 0 && ticks->ticks == 0);
        accel.set_external_clock(false);
        assert(!accel.waiting());
        assert(accel.step_quantum(1) == 1);
        assert(accel.accel_state().get_reg(0) == 42 && ticks->ticks == 2);
    }
    {
        ConvolutionAcceleratorSubsystem accel(4096);
        accel.accel_bus().write16(0x100, 0x202a);
        accel.accel_bus().write16(0x102, 0xdf00);
        accel.accel_state().set_pc(0x100);
        assert(accel.step_quantum(10) == 1);
        assert(accel.is_halted() && accel.step() == StepResult::Halt);
        accel.set_halted(false);
        accel.accel_state().set_pc(0x102);
        assert(accel.step() == StepResult::Halt);
    }
    {
        ConvolutionAcceleratorSubsystem accel(4096);
        accel.accel_state().set_pc(4096);
        assert(accel.step() == StepResult::Fault);
        assert(accel.is_halted() && accel.step_quantum(4) == 0);
    }
    {
        ConvolutionAcceleratorSubsystem accel(4096);
        accel.set_halted(true);
        assert(accel.step() == StepResult::Halt);
        assert(accel.step_quantum(10) == 0);
    }
}
