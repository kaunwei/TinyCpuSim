#pragma once

#include <memory>
#include <utility>
#include "tinyarmsim/cpu/interpreter.hpp"
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/dev/crc32.hpp"
#include "tinyarmsim/dev/custom_device.hpp"
#include "tinyarmsim/dev/dma.hpp"
#include "tinyarmsim/dev/gpio.hpp"
#include "tinyarmsim/dev/i2c.hpp"
#include "tinyarmsim/cpu/nvic.hpp"
#include "tinyarmsim/dev/spi.hpp"
#include "tinyarmsim/dev/timer.hpp"
#include "tinyarmsim/dev/uart.hpp"

namespace tinyarmsim::mmio {

struct SoCPeripherals {
    UartDevice* uart{nullptr};
    TimerDevice* timer{nullptr};
    I2CDevice* i2c{nullptr};
    SpiDevice* spi{nullptr};
    Crc32Device* crc{nullptr};
    GpioDevice* gpio{nullptr};
    DmaDevice* dma{nullptr};
    CustomDevice* custom{nullptr};
    NVICDevice* nvic{nullptr};

    static SoCPeripherals attach_all(MemoryBus& bus, IsaInterpreter& interp) {
        SoCPeripherals p{};
        auto uart_dev   = std::make_unique<UartDevice>();
        auto timer_dev  = std::make_unique<TimerDevice>();
        auto i2c_dev    = std::make_unique<I2CDevice>();
        auto spi_dev    = std::make_unique<SpiDevice>();
        auto crc_dev    = std::make_unique<Crc32Device>();
        auto gpio_dev   = std::make_unique<GpioDevice>();
        auto dma_dev    = std::make_unique<DmaDevice>(bus);
        auto custom_dev = std::make_unique<CustomDevice>();
        auto nvic_dev   = std::make_unique<NVICDevice>();

        p.uart   = uart_dev.get();
        p.timer  = timer_dev.get();
        p.i2c    = i2c_dev.get();
        p.spi    = spi_dev.get();
        p.crc    = crc_dev.get();
        p.gpio   = gpio_dev.get();
        p.dma    = dma_dev.get();
        p.custom = custom_dev.get();
        p.nvic   = nvic_dev.get();

        bus.register_device(std::move(uart_dev));
        bus.register_device(std::move(timer_dev));
        bus.register_device(std::move(i2c_dev));
        bus.register_device(std::move(spi_dev));
        bus.register_device(std::move(crc_dev));
        bus.register_device(std::move(gpio_dev));
        bus.register_device(std::move(dma_dev));
        bus.register_device(std::move(custom_dev));
        bus.register_device(std::move(nvic_dev));

        interp.attach_nvic(p.nvic);
        return p;
    }
};

} // namespace tinyarmsim::mmio
