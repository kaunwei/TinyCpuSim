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
#include "tinyarmsim/mmio/gpio.hpp"
#include "tinyarmsim/mmio/dma.hpp"
#include "tinyarmsim/mmio/soc_platform.hpp"
#include "tinyarmsim/loader.hpp"

using namespace tinyarmsim;

TEST(BaremetalFirmwareTest, FullPeripheralSystemExecution) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    IsaInterpreter interp(state, bus);

    auto soc = mmio::SoCPeripherals::attach_all(bus, interp);

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

    EXPECT_EQ(soc.uart->get_tx_output(), "OK");

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

TEST(BaremetalFirmwareTest, GpioDebounceAndDmaBurstWithInterruptsViaInterpreter) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    IsaInterpreter interp(state, bus);

    auto gpio = std::make_unique<mmio::GpioDevice>();
    auto dma = std::make_unique<mmio::DmaDevice>(bus);
    auto nvic = std::make_unique<mmio::NVICDevice>();

    mmio::GpioDevice* gpio_ptr = gpio.get();
    mmio::DmaDevice* dma_ptr = dma.get();
    mmio::NVICDevice* nvic_ptr = nvic.get();

    bus.register_device(std::move(gpio));
    bus.register_device(std::move(dma));
    bus.register_device(std::move(nvic));
    interp.attach_nvic(nvic_ptr);

    // Setup NVIC Vector Table at 0x1000:
    // IRQ 2 (GPIO0) -> offset (16 + 2)*4 = 0x48 -> 0x1048 -> ISR at 0x2000
    // IRQ 3 (DMA0)  -> offset (16 + 3)*4 = 0x4C -> 0x104C -> ISR at 0x2100
    nvic_ptr->set_vtor(0x1000);
    bus.write32(0x1048, 0x2000 | 1);
    bus.write32(0x104C, 0x2100 | 1);
    nvic_ptr->write(0x100, (1u << 2) | (1u << 3), mmio::AccessSize::Word);

    // GPIO0 ISR at 0x2000:
    // Clear GPIO0 ISR bit 4, set R6 = 1, and return
    // 0x2000: LDR R0, [PC, #8] (0x4802) -> load &GPIO0->ISR (0x40005014)
    // 0x2002: MOVS R1, #0x10   (0x2110)
    // 0x2004: STR R1, [R0, #0] (0x6001)
    // 0x2006: MOVS R6, #1      (0x2601)
    // 0x2008: BX LR            (0x4770)
    // 0x200A: NOP              (0xBF00)
    // 0x200C: 0x40005014
    bus.write16(0x2000, 0x4802);
    bus.write16(0x2002, 0x2110);
    bus.write16(0x2004, 0x6001);
    bus.write16(0x2006, 0x2601);
    bus.write16(0x2008, 0x4770);
    bus.write16(0x200A, 0xBF00);
    bus.write32(0x200C, 0x40005014);

    // DMA0 ISR at 0x2100:
    // Clear DMA0 SR bit 1 (TCIF), set R7 = 1, and return
    // 0x2100: LDR R0, [PC, #8] (0x4802) -> load &DMA0->SR (0x40006010)
    // 0x2102: MOVS R1, #2      (0x2102)
    // 0x2104: STR R1, [R0, #0] (0x6001)
    // 0x2106: MOVS R7, #1      (0x2701)
    // 0x2108: BX LR            (0x4770)
    // 0x210A: NOP              (0xBF00)
    // 0x210C: 0x40006010
    bus.write16(0x2100, 0x4802);
    bus.write16(0x2102, 0x2102);
    bus.write16(0x2104, 0x6001);
    bus.write16(0x2106, 0x2701);
    bus.write16(0x2108, 0x4770);
    bus.write16(0x210A, 0xBF00);
    bus.write32(0x210C, 0x40006010);

    // Main Thumb program at 0x0100:
    // Wait until both R6 != 0 (GPIO IRQ fired) and R7 != 0 (DMA IRQ fired), then exit with R0 = 0
    // 0x0100: CMP R6, #0  (0x2E00)
    // 0x0102: BEQ 0x0100  (0xD0FD)
    // 0x0104: CMP R7, #0  (0x2F00)
    // 0x0106: BEQ 0x0100  (0xD0FB)
    // 0x0108: MOVS R0, #0 (0x2000)
    // 0x010A: SVC #0      (0xDF00)
    bus.write16(0x0100, 0x2E00);
    bus.write16(0x0102, 0xD0FD);
    bus.write16(0x0104, 0x2F00);
    bus.write16(0x0106, 0xD0FB);
    bus.write16(0x0108, 0x2000);
    bus.write16(0x010A, 0xDF00);

    state.set_reg(13, 0x10000);
    state.set_pc(0x0100);
    state.set_reg(6, 0);
    state.set_reg(7, 0);

    // Configure GPIO0: pins [3:0] output, pin 4 input with IRQ enabled, DEBOUNCE = 6 ticks
    gpio_ptr->write(0x00, 0x0Fu, mmio::AccessSize::Word);
    gpio_ptr->write(0x10, (1u << 4), mmio::AccessSize::Word); // IER pin 4
    gpio_ptr->write(0x18, 6, mmio::AccessSize::Word);         // DEBOUNCE = 6 ticks
    gpio_ptr->inject_external_input(1u << 4);

    // Prepare 4 words in RAM at 0x3000 and configure DMA0 with Turbo CLKDIV = 2 (8 ticks total)
    for (uint32_t i = 0; i < 4; ++i) {
        bus.write32(0x3000 + i * 4, 0xCAFE0000u + i);
    }
    dma_ptr->write(0x14, 2, mmio::AccessSize::Word);      // CLKDIV = 2 ticks/word
    dma_ptr->write(0x00, 0x3000, mmio::AccessSize::Word); // SRC
    dma_ptr->write(0x04, 0x4000, mmio::AccessSize::Word); // DST
    dma_ptr->write(0x08, 4, mmio::AccessSize::Word);      // 4 words
    dma_ptr->write(0x0C, 0x2Fu, mmio::AccessSize::Word);  // EN | TCIE | SRC_INC | DST_INC | WORD

    // Run interpreter: step() drives bus.tick(1), completing GPIO debounce (6 ticks) & DMA (8 ticks)
    uint32_t exit_code = interp.run(200);
    EXPECT_EQ(exit_code, 0u);
    EXPECT_EQ(state.get_reg(6), 1u); // GPIO0 ISR executed
    EXPECT_EQ(state.get_reg(7), 1u); // DMA0 ISR executed

    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(bus.read32(0x4000 + i * 4), 0xCAFE0000u + i);
    }
    EXPECT_EQ(dma_ptr->read(0x10, mmio::AccessSize::Word) & 0x08u, 0u); // Zero DMA OVR
}

TEST(BaremetalFirmwareTest, FullCortexMBootAndBareMetalInstructionSuiteWithPrimask) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    IsaInterpreter interp(state, bus);

    auto soc = mmio::SoCPeripherals::attach_all(bus, interp);

    // Set up Cortex-M vector table at 0x00010000
    soc.nvic->set_vtor(0x00010000);
    bus.write32(0x00010000, 0x00030000);         // Initial MSP
    bus.write32(0x00010004, 0x00010100 | 1);     // Reset_Handler
    bus.write32(0x00010044, 0x00010200 | 1);     // Timer0 IRQ 1 vector (offset (16 + 1)*4 = 0x44)
    soc.nvic->write(0x100, (1u << 1), mmio::AccessSize::Word); // Enable IRQ 1

    Loader::boot_cortex_m(bus, state, 0x00010000);
    EXPECT_EQ(state.get_sp(), 0x00030000u);
    EXPECT_EQ(state.get_pc(), 0x00010100u);

    // Timer0 ISR at 0x00010200:
    // Clear Timer0 INTCLR (0x4000100C), set R5 = 77, BX LR
    bus.write16(0x00010200, 0x4802); // LDR R0, [PC, #8] -> 0x0001020C
    bus.write16(0x00010202, 0x2101); // MOVS R1, #1
    bus.write16(0x00010204, 0x6001); // STR R1, [R0, #0]
    bus.write16(0x00010206, 0x254D); // MOVS R5, #77
    bus.write16(0x00010208, 0x4770); // BX LR
    bus.write16(0x0001020A, 0xBF00); // NOP
    bus.write32(0x0001020C, 0x4000100C);

    // Bare-metal program at 0x00010100:
    // 0x00010100: CPSID i               (0xB672)
    // 0x00010102: SUB SP, SP, #16       (0xB084)
    // 0x00010104: ADD R0, SP, #4        (0xA801)
    // 0x00010106: PUSH.W {R4, R8, LR}   (0xE92D, 0x4110)
    // 0x0001010A: UDIV R1, R2, R3       (0xFBB2, 0xF1F3)
    // 0x0001010E: UXTB R1, R1           (0xB2C9)
    // 0x00010110: REV R4, R4            (0xBA24)
    // 0x00010112: DMB                   (0xF3BF, 0x8F5F)
    // 0x00010116: POP.W {R4, R8, LR}    (0xE8BD, 0x4110)
    // 0x0001011A: CPSIE i               (0xB662)
    // 0x0001011C: ADD SP, SP, #16       (0xB004)
    // 0x0001011E: MOVS R0, #0           (0x2000)
    // 0x00010120: SVC #0                (0xDF00)
    bus.write16(0x00010100, 0xB672);
    bus.write16(0x00010102, 0xB084);
    bus.write16(0x00010104, 0xA801);
    bus.write16(0x00010106, 0xE92D);
    bus.write16(0x00010108, 0x4110);
    bus.write16(0x0001010A, 0xFBB2);
    bus.write16(0x0001010C, 0xF1F3);
    bus.write16(0x0001010E, 0xB2C9);
    bus.write16(0x00010110, 0xBA24);
    bus.write16(0x00010112, 0xF3BF);
    bus.write16(0x00010114, 0x8F5F);
    bus.write16(0x00010116, 0xE8BD);
    bus.write16(0x00010118, 0x4110);
    bus.write16(0x0001011A, 0xB662);
    bus.write16(0x0001011C, 0xB004);
    bus.write16(0x0001011E, 0x2000);
    bus.write16(0x00010120, 0xDF00);

    state.set_reg(2, 0x1050);
    state.set_reg(3, 0x10);
    state.set_reg(4, 0x11223344);
    state.set_reg(5, 0);
    state.set_reg(8, 0xAABBCCDD);
    state.set_lr(0x00010501);

    // Step 1: CPSID i -> PRIMASK == 1
    interp.step();
    EXPECT_TRUE(state.get_primask());

    // Step 2 & 3: SUB SP, SP, #16; ADD R0, SP, #4
    interp.step();
    EXPECT_EQ(state.get_sp(), 0x0002FFF0u);
    interp.step();
    EXPECT_EQ(state.get_reg(0), 0x0002FFF4u);

    // Start Timer0 (LOAD = 2, PSC = 0)
    soc.timer->write(0x18, 0, mmio::AccessSize::Word);    // PSC = 0
    soc.timer->write(0x00, 2, mmio::AccessSize::Word);    // LOAD = 2
    soc.timer->write(0x08, 0x05, mmio::AccessSize::Word); // Enable | One-shot | IRQ_EN

    // Step 5 instructions while PRIMASK == 1:
    // 1. PUSH.W {R4, R8, LR}
    // 2. UDIV R1, R2, R3 -> 0x105
    // 3. UXTB R1, R1     -> 0x05
    // 4. REV R4, R4      -> 0x44332211
    // 5. DMB
    for (int i = 0; i < 5; ++i) {
        interp.step();
    }
    EXPECT_TRUE(state.get_primask());
    EXPECT_TRUE(soc.nvic->is_pending(1)); // Timer0 IRQ is pending in NVIC ISPR
    EXPECT_EQ(state.get_reg(5), 0u);      // Did NOT preempt while CPSID i is active!
    EXPECT_EQ(state.get_reg(1), 0x05u);

    // Step POP.W {R4, R8, LR} -> restores R4 to 0x11223344
    interp.step();
    EXPECT_EQ(state.get_reg(4), 0x11223344u);
    EXPECT_EQ(state.get_reg(8), 0xAABBCCDDu);

    // Run remaining program: CPSIE i unmasks IRQ 1, preempts into ISR (sets R5 = 77),
    // returns via BX LR, deallocates stack (ADD SP, SP, #16), and exits cleanly via SVC #0
    uint32_t exit_code = interp.run(50);
    EXPECT_EQ(exit_code, 0u);
    EXPECT_FALSE(state.get_primask());
    EXPECT_EQ(state.get_reg(5), 77u);
    EXPECT_EQ(state.get_sp(), 0x00030000u);
}
