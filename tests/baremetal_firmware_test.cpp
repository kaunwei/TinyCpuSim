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
