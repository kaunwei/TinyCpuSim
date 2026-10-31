#pragma once

#include <memory>
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/dev/uart.hpp"
#include "tinyarmsim/dev/timer.hpp"
#include "tinyarmsim/dev/gpio.hpp"
#include "tinyarmsim/platform/virtual_platform.hpp"
#include "tinyarmsim/platform/proxy_device.hpp"

namespace tinyarmsim::platform {

class BasicMcuPlatform {
public:
    static constexpr size_t MEMORY_SIZE = 64 * 1024 * 1024;
    static constexpr uint32_t UART_BASE = 0x40000000;
    static constexpr uint32_t TIMER_BASE = 0x40001000;
    static constexpr uint32_t GPIO_BASE = 0x40002000;
    static constexpr uint32_t NVIC_BASE = 0xE000E000;

    BasicMcuPlatform()
        : platform_(),
          bus_(platform_.create_bus("mcu_bus", MEMORY_SIZE)),
          cpu_(platform_.create_cpu("mcu_cpu", bus_)) {
        bus_.register_device(std::make_unique<ProxyMMIODevice>(cpu_.nvic, NVIC_BASE, 0x1000));
        bus_.register_device(std::make_unique<mmio::UartDevice>(UART_BASE, 0x1000));
        bus_.register_device(std::make_unique<mmio::TimerDevice>(TIMER_BASE, 0x1000));
        bus_.register_device(std::make_unique<mmio::GpioDevice>(GPIO_BASE, 0x1000));
    }

    [[nodiscard]] VirtualPlatform& platform() noexcept { return platform_; }
    [[nodiscard]] MemoryBus& bus() noexcept { return bus_; }
    [[nodiscard]] CpuInstance& cpu() noexcept { return cpu_; }

private:
    VirtualPlatform platform_;
    MemoryBus& bus_;
    CpuInstance& cpu_;
};

} // namespace tinyarmsim::platform
