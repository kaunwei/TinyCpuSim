#pragma once

#include <memory>
#include <string>
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/dev/uart.hpp"
#include "tinyarmsim/dev/timer.hpp"
#include "tinyarmsim/dev/gpio.hpp"
#include "tinyarmsim/dev/dma.hpp"
#include "tinyarmsim/platform/virtual_platform.hpp"
#include "tinyarmsim/platform/proxy_device.hpp"
#include "tinyarmsim/platform/timer_adapter.hpp"
#include "tinyarmsim/platform/uart_adapter.hpp"
#include "tinyarmsim/platform/dma_adapter.hpp"
#include <limits>

namespace tinyarmsim::platform {

class BasicMcuPlatform {
public:
    static constexpr size_t MEMORY_SIZE = 64 * 1024 * 1024;
    static constexpr uint32_t UART_BASE = 0x40000000;
    static constexpr uint32_t TIMER_BASE = 0x40001000;
    static constexpr uint32_t GPIO_BASE = 0x40002000;
    static constexpr uint32_t DMA_BASE = 0x40006000;
    static constexpr uint32_t NVIC_BASE = 0xE000E000;

    explicit BasicMcuPlatform(uint64_t uart_numerator = 1, uint64_t uart_denominator = 1,
                              uint64_t dma_numerator = 1, uint64_t dma_denominator = 1)
        : platform_(),
          bus_(platform_.create_bus("mcu_bus", MEMORY_SIZE)),
          cpu_(platform_.create_cpu("mcu_cpu", bus_)) {
        bus_.register_device(std::make_unique<ProxyMMIODevice>(cpu_.nvic, NVIC_BASE, 0x1000));
        auto uart = std::make_unique<mmio::UartDevice>(UART_BASE, 0x1000);
        uart_ = uart.get();
        bus_.register_device(std::move(uart));
        auto timer = std::make_unique<mmio::TimerDevice>(TIMER_BASE, 0x1000);
        timer_ = timer.get();
        bus_.register_device(std::move(timer));
        auto gpio = std::make_unique<mmio::GpioDevice>(GPIO_BASE, 0x1000);
        gpio_ = gpio.get();
        bus_.register_device(std::move(gpio));
        auto dma = std::make_unique<mmio::DmaDevice>(bus_, DMA_BASE, 0x1000);
        dma_ = dma.get();
        bus_.register_device(std::move(dma));
        uart_adapter_ = std::make_unique<UartAdapter>(scheduler_, *uart_, cpu_.nvic, uart_numerator, uart_denominator);
        dma_adapter_ = std::make_unique<DmaAdapter>(scheduler_, *dma_, cpu_.nvic, dma_numerator, dma_denominator);
        cpu_.interpreter.set_external_clock(true);
    }

    void load_firmware(const std::string& elf_path, uint32_t vtor = 0x00000000) {
        platform_.load_elf("mcu_cpu", elf_path);
        platform_.boot_cortex_m("mcu_cpu", vtor);
    }

    uint64_t run(uint64_t max_steps, uint64_t quantum = 500) {
        if (max_steps > std::numeric_limits<uint64_t>::max() - scheduler_.now())
            throw std::overflow_error("platform deadline overflow");
        return run_until(scheduler_.now() + max_steps, quantum).progress;
    }

    RunResult run_until(uint64_t deadline, uint64_t quantum = 500) {
        if (quantum == 0) throw std::invalid_argument("zero quantum");
        if (deadline < scheduler_.now()) throw std::invalid_argument("past deadline");
        refresh_devices();
        return scheduler_.run(deadline, quantum, [this] {
            if (cpu_.halted) return StepResult::Halt;
            if (cpu_.interpreter.waiting()) return StepResult::Wait;
            try {
                cpu_.interpreter.step();
                refresh_devices();
                return StepResult::Progress;
            } catch (const CpuFaultException& fault) {
                cpu_.halted = true;
                return fault.get_fault_type() == FaultType::SoftwareInterrupt ? StepResult::Halt : StepResult::Fault;
            }
        });
    }

    [[nodiscard]] uint64_t time() const noexcept { return scheduler_.now(); }

    [[nodiscard]] VirtualPlatform& platform() noexcept { return platform_; }
    [[nodiscard]] MemoryBus& bus() noexcept { return bus_; }
    [[nodiscard]] CpuInstance& cpu() noexcept { return cpu_; }
    [[nodiscard]] mmio::DmaDevice& dma() noexcept { return *dma_; }
    [[nodiscard]] const mmio::DmaDevice& dma() const noexcept { return *dma_; }
    [[nodiscard]] DmaAdapter& dma_adapter() noexcept { return *dma_adapter_; }
    [[nodiscard]] const DmaAdapter& dma_adapter() const noexcept { return *dma_adapter_; }

private:
    void refresh_devices() {
        if ((bus_.read32(TIMER_BASE + 8) & 1u) && bus_.read32(TIMER_BASE)) {
            if (!timer_adapter_) timer_adapter_ = std::make_unique<TimerAdapter>(scheduler_, *timer_, cpu_.nvic, 1, 1, true);
            else timer_adapter_->start();
        } else if (timer_adapter_) {
            timer_adapter_->stop();
        }
        if (timer_->has_pending_irq()) cpu_.nvic.set_pending(timer_->irq_line());
        if (uart_->has_pending_irq()) cpu_.nvic.set_pending(uart_->irq_line());
        if (gpio_->has_pending_irq()) cpu_.nvic.set_pending(gpio_->irq_line());
        if (dma_->has_pending_irq()) cpu_.nvic.set_pending(dma_->irq_line());
        uart_adapter_->refresh();
        dma_adapter_->refresh();
        if (!other_scheduled_ && gpio_->is_busy()) schedule_other();
        if (other_scheduled_ && !gpio_->is_busy()) {
            other_event_.cancel();
            other_scheduled_ = false;
        }
    }

    void schedule_other() {
        if (scheduler_.now() == std::numeric_limits<uint64_t>::max()) return;
        other_scheduled_ = true;
        other_event_ = scheduler_.schedule(scheduler_.now() + 1, EventPhase::Device, [this] {
            other_scheduled_ = false;
            gpio_->tick(1);
            irq_event_ = scheduler_.schedule(scheduler_.now(), EventPhase::Irq, [this] {
                if (uart_->has_pending_irq()) cpu_.nvic.set_pending(uart_->irq_line());
                if (gpio_->has_pending_irq()) cpu_.nvic.set_pending(gpio_->irq_line());
                if (dma_->has_pending_irq()) cpu_.nvic.set_pending(dma_->irq_line());
            });
            if (gpio_->is_busy()) schedule_other();
        });
    }
    VirtualPlatform platform_;
    MemoryBus& bus_;
    CpuInstance& cpu_;
    mmio::UartDevice* uart_{};
    mmio::TimerDevice* timer_{};
    mmio::GpioDevice* gpio_{};
    mmio::DmaDevice* dma_{};
    TimeScheduler scheduler_;
    std::unique_ptr<TimerAdapter> timer_adapter_;
    std::unique_ptr<UartAdapter> uart_adapter_;
    std::unique_ptr<DmaAdapter> dma_adapter_;
    TimeScheduler::Handle other_event_, irq_event_;
    bool other_scheduled_ = false;
};

} // namespace tinyarmsim::platform
