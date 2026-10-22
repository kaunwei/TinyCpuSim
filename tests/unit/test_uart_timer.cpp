#include <gtest/gtest.h>
#include <memory>
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/mmio/nvic.hpp"
#include "tinyarmsim/mmio/uart.hpp"
#include "tinyarmsim/mmio/timer.hpp"

using namespace tinyarmsim;

TEST(UartTest, WriteAndOutputCapture) {
    mmio::UartDevice uart;
    EXPECT_EQ(uart.base_address(), mmio::UartDevice::DEFAULT_BASE);
    EXPECT_EQ(uart.address_span(), mmio::UartDevice::DEFAULT_SPAN);
    EXPECT_EQ(uart.device_name(), "UART0");
    EXPECT_EQ(uart.irq_line(), 0u);

    uart.reset();

    uart.write(0x00, static_cast<uint32_t>('H'), mmio::AccessSize::Byte);
    uart.write(0x00, static_cast<uint32_t>('i'), mmio::AccessSize::Byte);
    uart.write(0x00, static_cast<uint32_t>('!'), mmio::AccessSize::Byte);

    EXPECT_EQ(uart.get_tx_output(), "Hi!");
    EXPECT_EQ(uart.read(0x04, mmio::AccessSize::Word) & 0x01u, 0x01u); // TX_READY
    EXPECT_EQ(uart.read(0x04, mmio::AccessSize::Word) & 0x04u, 0x04u); // TX_EMPTY
}

TEST(UartTest, RxInjectionAndInterrupt) {
    mmio::UartDevice uart;
    uart.reset();
    EXPECT_FALSE(uart.has_pending_irq());

    // Enable RX IRQ: Bit 2
    uart.write(0x08, 0x07, mmio::AccessSize::Word); // TX_EN | RX_EN | RX_IRQ_EN
    uart.inject_rx(static_cast<uint8_t>('A'));

    EXPECT_TRUE(uart.has_pending_irq());
    EXPECT_EQ(uart.read(0x00, mmio::AccessSize::Byte), static_cast<uint32_t>('A'));
    EXPECT_FALSE(uart.has_pending_irq()); // FIFO now empty
}

TEST(UartTest, RxStringInjectionAndStatus) {
    mmio::UartDevice uart;
    uart.reset();

    uart.inject_rx("Hello");
    EXPECT_EQ(uart.read(0x04, mmio::AccessSize::Word) & 0x02u, 0x02u); // RX_VALID

    EXPECT_EQ(uart.read(0x00, mmio::AccessSize::Byte), static_cast<uint32_t>('H'));
    EXPECT_EQ(uart.read(0x00, mmio::AccessSize::Byte), static_cast<uint32_t>('e'));
    EXPECT_EQ(uart.read(0x00, mmio::AccessSize::Byte), static_cast<uint32_t>('l'));
    EXPECT_EQ(uart.read(0x00, mmio::AccessSize::Byte), static_cast<uint32_t>('l'));
    EXPECT_EQ(uart.read(0x00, mmio::AccessSize::Byte), static_cast<uint32_t>('o'));

    EXPECT_EQ(uart.read(0x04, mmio::AccessSize::Word) & 0x02u, 0x00u); // RX empty
    EXPECT_EQ(uart.read(0x00, mmio::AccessSize::Byte), 0u);
}

TEST(TimerTest, DownCountAndAutoReload) {
    mmio::TimerDevice timer;
    EXPECT_EQ(timer.base_address(), mmio::TimerDevice::DEFAULT_BASE);
    EXPECT_EQ(timer.address_span(), mmio::TimerDevice::DEFAULT_SPAN);
    EXPECT_EQ(timer.device_name(), "Timer0");
    EXPECT_EQ(timer.irq_line(), 1u);

    timer.reset();

    timer.write(0x00, 100, mmio::AccessSize::Word); // Load = 100
    // Control: Enable (bit 0) | Periodic (bit 1) | IRQ Enable (bit 2) = 0x07
    timer.write(0x08, 0x07, mmio::AccessSize::Word);

    EXPECT_EQ(timer.read(0x04, mmio::AccessSize::Word), 100u);
    EXPECT_FALSE(timer.has_pending_irq());

    // Tick 40 cycles
    timer.tick(40);
    EXPECT_EQ(timer.read(0x04, mmio::AccessSize::Word), 60u);
    EXPECT_FALSE(timer.has_pending_irq());

    // Tick 60 more cycles -> underflow & auto-reload
    timer.tick(60);
    EXPECT_TRUE(timer.has_pending_irq());
    EXPECT_EQ(timer.read(0x04, mmio::AccessSize::Word), 100u);

    // Clear interrupt
    timer.write(0x0C, 1, mmio::AccessSize::Word);
    EXPECT_FALSE(timer.has_pending_irq());
}

TEST(TimerTest, OneShotModeAndDisabling) {
    mmio::TimerDevice timer;
    timer.reset();

    timer.write(0x00, 50, mmio::AccessSize::Word); // Load = 50
    // Control: Enable (bit 0) | IRQ Enable (bit 2), no periodic bit = 0x05
    timer.write(0x08, 0x05, mmio::AccessSize::Word);

    timer.tick(50);
    EXPECT_TRUE(timer.has_pending_irq());
    EXPECT_EQ(timer.read(0x04, mmio::AccessSize::Word), 0u);
    // Control bit 0 (Enable) should now be cleared
    EXPECT_EQ(timer.read(0x08, mmio::AccessSize::Word) & 0x01u, 0u);

    // Additional tick should not do anything because timer disabled
    timer.tick(50);
    EXPECT_EQ(timer.read(0x04, mmio::AccessSize::Word), 0u);
}

TEST(UartTimerNvicTest, IrqBindingToNvic) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    IsaInterpreter interp(state, bus);

    auto nvic_dev = std::make_unique<mmio::NVICDevice>();
    mmio::NVICDevice* nvic = nvic_dev.get();
    bus.register_device(std::move(nvic_dev));
    interp.attach_nvic(nvic);

    auto uart_dev = std::make_unique<mmio::UartDevice>();
    mmio::UartDevice* uart = uart_dev.get();
    bus.register_device(std::move(uart_dev));

    auto timer_dev = std::make_unique<mmio::TimerDevice>();
    mmio::TimerDevice* timer = timer_dev.get();
    bus.register_device(std::move(timer_dev));

    EXPECT_EQ(uart->irq_line(), 0u);
    EXPECT_EQ(timer->irq_line(), 1u);

    // Enable IRQ 0 and IRQ 1 on NVIC (bit 0 and bit 1)
    nvic->write(0x100, 0x03, mmio::AccessSize::Word);

    // Trigger UART RX IRQ
    uart->write(0x08, 0x07, mmio::AccessSize::Word);
    uart->inject_rx(static_cast<uint8_t>('X'));
    EXPECT_TRUE(uart->has_pending_irq());

    for (const auto& slot : bus.mmio_devices()) {
        if (slot.dev && slot.dev->has_pending_irq()) {
            nvic->set_pending(slot.dev->irq_line());
        }
    }
    EXPECT_TRUE(nvic->is_pending(0));
    EXPECT_FALSE(nvic->is_pending(1));

    // Clear UART pending by reading the byte
    EXPECT_EQ(uart->read(0x00, mmio::AccessSize::Byte), static_cast<uint32_t>('X'));
    EXPECT_FALSE(uart->has_pending_irq());

    // Trigger Timer IRQ
    timer->write(0x00, 50, mmio::AccessSize::Word);
    timer->write(0x08, 0x07, mmio::AccessSize::Word);
    timer->tick(50);
    EXPECT_TRUE(timer->has_pending_irq());

    for (const auto& slot : bus.mmio_devices()) {
        if (slot.dev && slot.dev->has_pending_irq()) {
            nvic->set_pending(slot.dev->irq_line());
        }
    }
    EXPECT_TRUE(nvic->is_pending(1));
}
