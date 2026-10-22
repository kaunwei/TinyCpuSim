#include <gtest/gtest.h>
#include <memory>
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/instruction.hpp"
#include "tinyarmsim/mmio/uart.hpp"
#include "tinyarmsim/mmio/timer.hpp"
#include "tinyarmsim/mmio/i2c.hpp"
#include "tinyarmsim/mmio/spi.hpp"
#include "tinyarmsim/mmio/crc32.hpp"
#include "tinyarmsim/mmio/custom_device.hpp"
#include "tinyarmsim/mmio/nvic.hpp"

using namespace tinyarmsim;

TEST(BaremetalFirmwareTest, FullPeripheralSystemExecution) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    IsaInterpreter interp(state, bus);

    auto uart = std::make_unique<mmio::UartDevice>();
    auto timer = std::make_unique<mmio::TimerDevice>();
    auto i2c = std::make_unique<mmio::I2CDevice>();
    auto spi = std::make_unique<mmio::SpiDevice>();
    auto crc = std::make_unique<mmio::Crc32Device>();
    auto custom = std::make_unique<mmio::CustomDevice>();
    auto nvic = std::make_unique<mmio::NVICDevice>();

    mmio::UartDevice* uart_ptr = uart.get();
    [[maybe_unused]] mmio::CustomDevice* custom_ptr = custom.get();
    [[maybe_unused]] mmio::Crc32Device* crc_ptr = crc.get();
    mmio::NVICDevice* nvic_ptr = nvic.get();
    (void)custom_ptr;
    (void)crc_ptr;

    bus.register_device(std::move(uart));
    bus.register_device(std::move(timer));
    bus.register_device(std::move(i2c));
    bus.register_device(std::move(spi));
    bus.register_device(std::move(crc));
    bus.register_device(std::move(custom));
    bus.register_device(std::move(nvic));

    interp.attach_nvic(nvic_ptr);

    // 1. Test UART MMIO via Interpreter
    state.set_reg(1, 0x40000000); // UART0 DATA
    state.set_reg(2, 'O');
    DecodedInstruction str_o{};
    str_o.op = Opcode::STR;
    str_o.rd = 2;
    str_o.rn = 1;
    str_o.mem_size = 4;
    interp.execute(str_o);

    state.set_reg(2, 'K');
    DecodedInstruction str_k{};
    str_k.op = Opcode::STR;
    str_k.rd = 2;
    str_k.rn = 1;
    str_k.mem_size = 4;
    interp.execute(str_k);

    EXPECT_EQ(uart_ptr->get_tx_output(), "OK");

    // 2. Test Custom Accel via bus
    bus.write32(0x50000000, 10); // A = 10
    bus.write32(0x50000004, 32); // B = 32
    bus.write32(0x50000008, 1);  // CMD = ADD
    EXPECT_EQ(bus.read32(0x5000000C), 42u);

    // 3. Test CRC32 via bus
    bus.write32(0x40004008, 1); // Reset
    bus.write32(0x40004000, 0x12345678);
    EXPECT_NE(bus.read32(0x40004000), 0xFFFFFFFFu);

    // 4. Test SVC exit cleanly returns R0
    state.set_reg(0, 0); // Exit code 0
    DecodedInstruction svc_op{};
    svc_op.op = Opcode::SVC;
    EXPECT_THROW(interp.execute(svc_op), CpuFaultException);
}

TEST(BaremetalFirmwareTest, ClockedPeripheralsAndTimerInterruptViaInterpreter) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    IsaInterpreter interp(state, bus);

    auto uart = std::make_unique<mmio::UartDevice>();
    auto timer = std::make_unique<mmio::TimerDevice>();
    auto i2c = std::make_unique<mmio::I2CDevice>();
    auto spi = std::make_unique<mmio::SpiDevice>();
    auto custom = std::make_unique<mmio::CustomDevice>();
    auto nvic = std::make_unique<mmio::NVICDevice>();

    mmio::UartDevice* uart_ptr = uart.get();
    mmio::TimerDevice* timer_ptr = timer.get();
    mmio::I2CDevice* i2c_ptr = i2c.get();
    mmio::SpiDevice* spi_ptr = spi.get();
    mmio::CustomDevice* custom_ptr = custom.get();
    mmio::NVICDevice* nvic_ptr = nvic.get();

    bus.register_device(std::move(uart));
    bus.register_device(std::move(timer));
    bus.register_device(std::move(i2c));
    bus.register_device(std::move(spi));
    bus.register_device(std::move(custom));
    bus.register_device(std::move(nvic));
    interp.attach_nvic(nvic_ptr);

    // Enable Turbo Clock Dividers (1 CPU instruction tick = 1 protocol bit)
    uart_ptr->write(0x10, 1, mmio::AccessSize::Word);   // 10 ticks per 8N1 char
    spi_ptr->write(0x10, 1, mmio::AccessSize::Word);    // 8 ticks per SPI byte
    i2c_ptr->write(0x10, 1, mmio::AccessSize::Word);    // 9 ticks per I2C byte
    custom_ptr->write(0x14, 3, mmio::AccessSize::Word); // 3 ticks per Custom op

    // Set up NVIC Vector Table at 0x1000 for Timer0 (IRQ 1 -> 0x1044)
    nvic_ptr->set_vtor(0x1000);
    bus.write32(0x1044, 0x2000 | 1);
    nvic_ptr->write(0x100, (1u << 1), mmio::AccessSize::Word);

    // Timer ISR at 0x2000:
    // Clear TIMER0 INTCLR, flag that ISR fired in R5, and return
    // 0x2000: LDR R0, [PC, #8] (0x4802) -> load &TIMER0->INTCLR (0x4000100C)
    // 0x2002: MOVS R1, #1      (0x2101)
    // 0x2004: STR R1, [R0, #0] (0x6001) -> INTCLR = 1
    // 0x2006: MOVS R5, #1      (0x2501) -> flag that ISR fired
    // 0x2008: BX LR            (0x4770)
    // 0x200A: NOP              (0xBF00) -> align literal pool
    // 0x200C: 0x4000100C
    bus.write16(0x2000, 0x4802);
    bus.write16(0x2002, 0x2101);
    bus.write16(0x2004, 0x6001);
    bus.write16(0x2006, 0x2501);
    bus.write16(0x2008, 0x4770);
    bus.write16(0x200A, 0xBF00);
    bus.write32(0x200C, 0x4000100C);

    // Main Thumb program at 0x0100:
    // Poll loop waiting for R5 != 0 (CMP R5, #0; BEQ -4; MOVS R0, #0; SVC #0)
    // 0x0100: CMP R5, #0  (0x2D00)
    // 0x0102: BEQ 0x0100  (0xD0FD)
    // 0x0104: MOVS R0, #0 (0x2000)
    // 0x0106: SVC #0      (0xDF00)
    bus.write16(0x0100, 0x2D00);
    bus.write16(0x0102, 0xD0FD);
    bus.write16(0x0104, 0x2000);
    bus.write16(0x0106, 0xDF00);

    state.set_reg(13, 0x10000);
    state.set_pc(0x0100);
    state.set_reg(5, 0);

    // Kick off concurrent peripheral operations and Timer0 (PSC=1, LOAD=8 -> 16 ticks)
    uart_ptr->write(0x00, 'T', mmio::AccessSize::Byte);
    spi_ptr->write(0x0C, 0, mmio::AccessSize::Word);
    spi_ptr->write(0x08, 0x9F, mmio::AccessSize::Byte);
    i2c_ptr->write(0x0C, 0x50, mmio::AccessSize::Word);
    i2c_ptr->write(0x00, 0x01, mmio::AccessSize::Word); // START (2 ticks)
    custom_ptr->write(0x00, 20, mmio::AccessSize::Word);
    custom_ptr->write(0x04, 22, mmio::AccessSize::Word);
    custom_ptr->write(0x08, 1, mmio::AccessSize::Word);  // ADD (3 ticks)

    timer_ptr->write(0x18, 1, mmio::AccessSize::Word);   // PSC = 1
    timer_ptr->write(0x00, 8, mmio::AccessSize::Word);   // LOAD = 8 (16 ticks)
    timer_ptr->write(0x08, 0x05, mmio::AccessSize::Word); // Enable | One-shot | IRQ_EN

    // Run interpreter: polling loop executes until Timer0 ISR sets R5=1 after 16 ticks, then exits via SVC
    uint32_t exit_code = interp.run(200);
    EXPECT_EQ(exit_code, 0u);
    EXPECT_EQ(state.get_reg(5), 1u);

    // Verify all clocked peripherals completed their transfers without OVR errors
    EXPECT_EQ(uart_ptr->get_tx_output(), "T");
    EXPECT_EQ(uart_ptr->read(0x04, mmio::AccessSize::Word) & 0x10u, 0u); // No UART OVR
    EXPECT_EQ(spi_ptr->read(0x08, mmio::AccessSize::Byte), 0xEFu);
    EXPECT_EQ(spi_ptr->read(0x04, mmio::AccessSize::Word) & 0x08u, 0u);  // No SPI OVR
    EXPECT_EQ(custom_ptr->read(0x0C, mmio::AccessSize::Word), 42u);
    EXPECT_EQ(custom_ptr->read(0x10, mmio::AccessSize::Word) & 0x04u, 0u); // No Custom OVR
}
