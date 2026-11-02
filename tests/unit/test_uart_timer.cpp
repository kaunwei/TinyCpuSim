#include <gtest/gtest.h>
#include <memory>
#include "tinyarmsim/cpu/interpreter.hpp"
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/cpu/nvic.hpp"
#include "tinyarmsim/dev/uart.hpp"
#include "tinyarmsim/dev/timer.hpp"

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

TEST(UartTest, TryInjectRxCapacityAndBackpressure) {
    mmio::UartDevice uart(mmio::UartDevice::DEFAULT_BASE, mmio::UartDevice::DEFAULT_SPAN, 4);
    EXPECT_EQ(uart.rx_capacity(), 4u);
    EXPECT_EQ(uart.rx_size(), 0u);

    EXPECT_TRUE(uart.try_inject_rx(static_cast<uint8_t>('1')));
    EXPECT_TRUE(uart.try_inject_rx(static_cast<uint8_t>('2')));
    EXPECT_TRUE(uart.try_inject_rx(static_cast<uint8_t>('3')));
    EXPECT_TRUE(uart.try_inject_rx(static_cast<uint8_t>('4')));
    EXPECT_EQ(uart.rx_size(), 4u);

    // Fifo full: rejection without drop or overwrite
    EXPECT_FALSE(uart.try_inject_rx(static_cast<uint8_t>('5')));
    EXPECT_EQ(uart.rx_size(), 4u);

    // Reading frees capacity
    EXPECT_EQ(uart.read(0x00, mmio::AccessSize::Byte), static_cast<uint32_t>('1'));
    EXPECT_EQ(uart.rx_size(), 3u);
    EXPECT_TRUE(uart.try_inject_rx(static_cast<uint8_t>('5')));
    EXPECT_EQ(uart.rx_size(), 4u);
    EXPECT_FALSE(uart.try_inject_rx(static_cast<uint8_t>('6')));
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

TEST(TimerTest, PrescalerDividesBusClock) {
    mmio::TimerDevice timer;
    timer.reset();

    // PSC = 3 (divide by 4), LOAD = 10 -> requires 40 bus ticks to underflow
    timer.write(0x18, 3, mmio::AccessSize::Word);
    timer.write(0x00, 10, mmio::AccessSize::Word);
    timer.write(0x08, 0x07, mmio::AccessSize::Word); // Enable | Periodic | IRQ

    timer.tick(19);
    EXPECT_EQ(timer.read(0x04, mmio::AccessSize::Word), 6u); // 19 / 4 = 4 decrements -> 10 - 4 = 6
    EXPECT_FALSE(timer.has_pending_irq());

    timer.tick(21); // Total 40 ticks -> 10 decrements -> underflow
    EXPECT_TRUE(timer.has_pending_irq());
    EXPECT_EQ(timer.read(0x04, mmio::AccessSize::Word), 10u);
}

TEST(UartTest, TurboClockTimingAndOverrunProtection) {
    mmio::UartDevice uart;
    uart.reset();

    // Set Turbo CLKDIV = 2 -> 8N1 frame (10 bits) takes 20 ticks
    uart.write(0x10, 2, mmio::AccessSize::Word);
    uart.write(0x00, 'A', mmio::AccessSize::Byte);

    // Immediately after write: BUSY=1 (bit 3), TX_READY=0 (bit 0), output still empty
    uint32_t st = uart.read(0x04, mmio::AccessSize::Word);
    EXPECT_EQ(st & 0x01u, 0u);
    EXPECT_NE(st & 0x08u, 0u);
    EXPECT_EQ(uart.get_tx_output(), "");

    // Colliding write after 10 ticks triggers OVR (bit 4) and is dropped
    uart.tick(10);
    uart.write(0x00, 'X', mmio::AccessSize::Byte);
    st = uart.read(0x04, mmio::AccessSize::Word);
    EXPECT_NE(st & 0x10u, 0u); // OVR = 1

    // Finish remaining 10 ticks -> 'A' emitted, 'X' ignored
    uart.tick(10);
    st = uart.read(0x04, mmio::AccessSize::Word);
    EXPECT_NE(st & 0x01u, 0u); // TX_READY = 1
    EXPECT_EQ(st & 0x08u, 0u); // BUSY = 0
    EXPECT_EQ(uart.get_tx_output(), "A");

    // Clear OVR via write-1-to-clear
    uart.write(0x04, 0x10u, mmio::AccessSize::Word);
    EXPECT_EQ(uart.read(0x04, mmio::AccessSize::Word) & 0x10u, 0u);
}

TEST(TimerTest, InterpreterStepAutomaticallyTicksTimerAndTriggersISR) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    IsaInterpreter interp(state, bus);

    auto timer_dev = std::make_unique<mmio::TimerDevice>();
    auto nvic_dev = std::make_unique<mmio::NVICDevice>();
    mmio::TimerDevice* timer = timer_dev.get();
    mmio::NVICDevice* nvic = nvic_dev.get();

    bus.register_device(std::move(timer_dev));
    bus.register_device(std::move(nvic_dev));
    interp.attach_nvic(nvic);

    // Vector table at 0x1000; Timer0 is IRQ 1 -> offset (16 + 1)*4 = 0x44 -> 0x1044
    nvic->set_vtor(0x1000);
    bus.write32(0x1044, 0x2000 | 1);
    nvic->write(0x100, (1u << 1), mmio::AccessSize::Word); // Enable IRQ 1

    // ISR at 0x2000:
    // MOVS R4, #99 (0x2463)
    // BX LR        (0x4770)
    bus.write16(0x2000, 0x2463);
    bus.write16(0x2002, 0x4770);

    // Main loop at 0x0100: fill with NOPs (MOV R8, R8 = 0x46C0)
    for (uint32_t addr = 0x0100; addr < 0x0140; addr += 2) {
        bus.write16(addr, 0x46C0);
    }

    state.set_reg(13, 0x10000); // SP
    state.set_pc(0x0100);
    state.set_reg(4, 0);

    // Configure Timer0: PSC = 1 (2 ticks per count), LOAD = 3 -> fires after 6 steps
    timer->write(0x18, 1, mmio::AccessSize::Word);
    timer->write(0x00, 3, mmio::AccessSize::Word);
    timer->write(0x08, 0x05, mmio::AccessSize::Word); // Enable | One-shot | IRQ Enable

    for (int i = 0; i < 12; ++i) {
        interp.step();
    }

    EXPECT_EQ(state.get_reg(4), 99u);
}

TEST(TimerTest, SkippedItSlotTicksOnceAndDeliversPendingIrqAtAdvancedPc) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    IsaInterpreter interp(state, bus);
    auto timer_dev = std::make_unique<mmio::TimerDevice>();
    auto nvic_dev = std::make_unique<mmio::NVICDevice>();
    mmio::TimerDevice* timer = timer_dev.get();
    mmio::NVICDevice* nvic = nvic_dev.get();
    bus.register_device(std::move(timer_dev));
    bus.register_device(std::move(nvic_dev));
    interp.attach_nvic(nvic);
    nvic->set_vtor(0x1000);
    bus.write32(0x1044, 0x2001);
    nvic->write(0x100, 1u << 1, mmio::AccessSize::Word);
    bus.write16(0x1000, 0xDE00);
    bus.write16(0x2000, 0xBF00);
    state.set_pc(0x1000);
    state.set_reg(13, 0x8000);
    state.set_it_state(0x08);
    state.set_flag_z(false);
    timer->write(0x00, 1, mmio::AccessSize::Word);
    timer->write(0x08, 0x05, mmio::AccessSize::Word);

    interp.step();

    EXPECT_EQ(timer->read(0x04, mmio::AccessSize::Word), 0u);
    EXPECT_EQ(timer->read(0x10, mmio::AccessSize::Word), 1u);
    EXPECT_EQ(state.get_pc(), 0x1002u);
    EXPECT_EQ(state.get_it_state(), 0u);
    interp.step();
    EXPECT_EQ(state.get_pc(), 0x2000u);
    const uint32_t exception_sp = state.get_reg(13);
    EXPECT_EQ(state.get_reg(14), mmio::NVICDevice::EXC_RETURN_THREAD_MSP);
    EXPECT_EQ(state.get_it_state(), 0u);
    EXPECT_EQ(bus.read32(exception_sp + 24), 0x1002u);
    EXPECT_EQ(bus.read32(exception_sp + 28), state.get_cpsr());
}
