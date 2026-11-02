#include <gtest/gtest.h>
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/dev/i2c.hpp"
#include "tinyarmsim/dev/spi.hpp"
#include "tinyarmsim/dev/crc32.hpp"
#include "tinyarmsim/dev/custom_device.hpp"
#include "tinyarmsim/dev/gpio.hpp"
#include "tinyarmsim/dev/dma.hpp"

using namespace tinyarmsim;

TEST(I2CTest, EEPROMWriteAndRead) {
    mmio::I2CDevice i2c;
    EXPECT_EQ(i2c.base_address(), mmio::I2CDevice::DEFAULT_BASE);
    EXPECT_EQ(i2c.address_span(), mmio::I2CDevice::DEFAULT_SPAN);
    EXPECT_EQ(i2c.device_name(), "I2C0");

    i2c.reset();
    EXPECT_EQ(i2c.read(0x04, mmio::AccessSize::Word), 0x08u); // TXE = 1

    // 1. Set slave address to 0x50
    i2c.write(0x0C, 0x50, mmio::AccessSize::Word);
    EXPECT_EQ(i2c.read(0x0C, mmio::AccessSize::Word), 0x50u);

    // 2. Issue START
    i2c.write(0x00, 0x01, mmio::AccessSize::Word);
    // 3. Send EEPROM word offset 0x10
    i2c.write(0x08, 0x10, mmio::AccessSize::Byte);
    // 4. Write data bytes 'A', 'B'
    i2c.write(0x08, 'A', mmio::AccessSize::Byte);
    i2c.write(0x08, 'B', mmio::AccessSize::Byte);
    // 5. Issue STOP
    i2c.write(0x00, 0x02, mmio::AccessSize::Word);

    EXPECT_EQ(i2c.read_eeprom_direct(0x10), static_cast<uint8_t>('A'));
    EXPECT_EQ(i2c.read_eeprom_direct(0x11), static_cast<uint8_t>('B'));

    // Read back via DR: set address pointer to 0x10 and trigger read
    i2c.write(0x00, 0x01, mmio::AccessSize::Word); // START
    i2c.write(0x08, 0x10, mmio::AccessSize::Byte); // pointer 0x10
    i2c.write(0x00, 0x04, mmio::AccessSize::Word); // Read trigger (CR bit 2)
    EXPECT_TRUE(i2c.read(0x04, mmio::AccessSize::Word) & 0x04u); // RXNE = 1
    EXPECT_EQ(i2c.read(0x08, mmio::AccessSize::Byte), static_cast<uint8_t>('A'));
    EXPECT_FALSE(i2c.read(0x04, mmio::AccessSize::Word) & 0x04u); // RXNE cleared
    i2c.write(0x00, 0x02, mmio::AccessSize::Word); // STOP
}

TEST(SpiTest, JedecIdRead) {
    mmio::SpiDevice spi;
    EXPECT_EQ(spi.base_address(), mmio::SpiDevice::DEFAULT_BASE);
    EXPECT_EQ(spi.address_span(), mmio::SpiDevice::DEFAULT_SPAN);
    EXPECT_EQ(spi.device_name(), "SPI0");

    spi.reset();

    // Assert CS: CS = 0
    spi.write(0x0C, 0, mmio::AccessSize::Word);
    EXPECT_EQ(spi.read(0x0C, mmio::AccessSize::Word), 0u);

    // Send 0x9F (Read ID)
    spi.write(0x08, 0x9F, mmio::AccessSize::Byte);
    EXPECT_EQ(spi.read(0x08, mmio::AccessSize::Byte), 0xEFu);

    // Dummy clock 1: Memory Type
    spi.write(0x08, 0x00, mmio::AccessSize::Byte);
    EXPECT_EQ(spi.read(0x08, mmio::AccessSize::Byte), 0x40u);

    // Dummy clock 2: Capacity
    spi.write(0x08, 0x00, mmio::AccessSize::Byte);
    EXPECT_EQ(spi.read(0x08, mmio::AccessSize::Byte), 0x18u);

    // Deassert CS: CS = 1
    spi.write(0x0C, 1, mmio::AccessSize::Word);
    EXPECT_EQ(spi.read(0x0C, mmio::AccessSize::Word), 1u);

    // With CS deasserted, transfer should return 0xFF
    spi.write(0x08, 0x9F, mmio::AccessSize::Byte);
    EXPECT_EQ(spi.read(0x08, mmio::AccessSize::Byte), 0xFFu);
}

TEST(Crc32Test, KnownVectorCalculation) {
    mmio::Crc32Device crc;
    EXPECT_EQ(crc.base_address(), mmio::Crc32Device::DEFAULT_BASE);
    EXPECT_EQ(crc.address_span(), mmio::Crc32Device::DEFAULT_SPAN);
    EXPECT_EQ(crc.device_name(), "CRC32");

    crc.reset();
    EXPECT_EQ(crc.read(0x00, mmio::AccessSize::Word), 0xFFFFFFFFu);
    EXPECT_EQ(crc.read(0x04, mmio::AccessSize::Word), 0xFFFFFFFFu);

    // Calculate CRC of "1234" byte-by-byte
    const char* data = "1234";
    for (int i = 0; i < 4; ++i) {
        crc.write(0x00, static_cast<uint8_t>(data[i]), mmio::AccessSize::Byte);
    }
    uint32_t crc_val = crc.read(0x00, mmio::AccessSize::Word);
    EXPECT_NE(crc_val, 0xFFFFFFFFu);

    // Reset via CR bit 0
    crc.write(0x08, 0x01, mmio::AccessSize::Word);
    EXPECT_EQ(crc.read(0x00, mmio::AccessSize::Word), 0xFFFFFFFFu);

    // Word write of the same 4 bytes ("1234" little endian: '1' | ('2'<<8) | ('3'<<16) | ('4'<<24))
    uint32_t word_val = static_cast<uint8_t>('1') |
                        (static_cast<uint8_t>('2') << 8) |
                        (static_cast<uint8_t>('3') << 16) |
                        (static_cast<uint8_t>('4') << 24);
    crc.write(0x00, word_val, mmio::AccessSize::Word);
    EXPECT_EQ(crc.read(0x00, mmio::AccessSize::Word), crc_val);
}

TEST(CustomDeviceTest, AccelMathOperations) {
    mmio::CustomDevice custom;
    EXPECT_EQ(custom.base_address(), mmio::CustomDevice::DEFAULT_BASE);
    EXPECT_EQ(custom.address_span(), mmio::CustomDevice::DEFAULT_SPAN);
    EXPECT_EQ(custom.device_name(), "CustomAccel");

    custom.reset();

    // 1. ADD test: 15 + 27 = 42
    custom.write(0x00, 15, mmio::AccessSize::Word); // A = 15
    custom.write(0x04, 27, mmio::AccessSize::Word); // B = 27
    custom.write(0x08, 1, mmio::AccessSize::Word);  // CMD = ADD

    EXPECT_EQ(custom.read(0x0C, mmio::AccessSize::Word), 42u);
    EXPECT_EQ(custom.read(0x10, mmio::AccessSize::Word) & 0x01, 0x01u); // DONE

    // 2. MUL test: 6 * 7 = 42
    custom.write(0x00, 6, mmio::AccessSize::Word);
    custom.write(0x04, 7, mmio::AccessSize::Word);
    custom.write(0x08, 2, mmio::AccessSize::Word);  // CMD = MUL

    EXPECT_EQ(custom.read(0x0C, mmio::AccessSize::Word), 42u);
    EXPECT_EQ(custom.read(0x10, mmio::AccessSize::Word) & 0x01, 0x01u);

    // 3. XOR test: 0xAA ^ 0x55 = 0xFF
    custom.write(0x00, 0xAA, mmio::AccessSize::Word);
    custom.write(0x04, 0x55, mmio::AccessSize::Word);
    custom.write(0x08, 3, mmio::AccessSize::Word);  // CMD = XOR

    EXPECT_EQ(custom.read(0x0C, mmio::AccessSize::Word), 0xFFu);
    EXPECT_EQ(custom.read(0x10, mmio::AccessSize::Word) & 0x01, 0x01u);

    // Reset verification
    custom.reset();
    EXPECT_EQ(custom.read(0x00, mmio::AccessSize::Word), 0u);
    EXPECT_EQ(custom.read(0x04, mmio::AccessSize::Word), 0u);
    EXPECT_EQ(custom.read(0x08, mmio::AccessSize::Word), 0u);
    EXPECT_EQ(custom.read(0x0C, mmio::AccessSize::Word), 0u);
    EXPECT_EQ(custom.read(0x10, mmio::AccessSize::Word), 0u);
}

TEST(I2CTest, TurboClockTimingAndOverrunProtection) {
    mmio::I2CDevice i2c;
    i2c.reset();

    // Set Turbo CLKDIV = 2 (START = 4 ticks, Byte = 18 ticks)
    i2c.write(0x10, 2, mmio::AccessSize::Word);
    i2c.write(0x0C, 0x50, mmio::AccessSize::Word);

    // Issue START -> 2 * 2 = 4 ticks busy
    i2c.write(0x00, 0x01, mmio::AccessSize::Word);
    EXPECT_NE(i2c.read(0x04, mmio::AccessSize::Word) & 0x01u, 0u); // BUSY
    i2c.tick(4);
    EXPECT_EQ(i2c.read(0x04, mmio::AccessSize::Word) & 0x01u, 0u); // Idle

    // Send EEPROM address offset 0x20 -> 9 * 2 = 18 ticks
    i2c.write(0x08, 0x20, mmio::AccessSize::Byte);
    EXPECT_NE(i2c.read(0x04, mmio::AccessSize::Word) & 0x01u, 0u); // BUSY

    // Colliding write at tick 10 sets OVR (bit 4)
    i2c.tick(10);
    i2c.write(0x08, 0x99, mmio::AccessSize::Byte);
    EXPECT_NE(i2c.read(0x04, mmio::AccessSize::Word) & 0x10u, 0u); // OVR = 1

    // Complete remaining 8 ticks
    i2c.tick(8);
    EXPECT_EQ(i2c.read(0x04, mmio::AccessSize::Word) & 0x01u, 0u); // BUSY = 0

    // Clear OVR and write data byte 0x77 properly
    i2c.write(0x04, 0x10u, mmio::AccessSize::Word);
    EXPECT_EQ(i2c.read(0x04, mmio::AccessSize::Word) & 0x10u, 0u);

    i2c.write(0x08, 0x77, mmio::AccessSize::Byte);
    i2c.tick(18);
    EXPECT_EQ(i2c.read_eeprom_direct(0x20), 0x77u);
}

TEST(SpiTest, TurboClockTimingAndOverrunProtection) {
    mmio::SpiDevice spi;
    spi.reset();

    // Set Turbo CLKDIV = 2 -> 8 SCLK bits * 2 = 16 ticks per byte
    spi.write(0x10, 2, mmio::AccessSize::Word);
    spi.write(0x0C, 0, mmio::AccessSize::Word); // CS = 0

    spi.write(0x08, 0x9F, mmio::AccessSize::Byte);
    uint32_t sr = spi.read(0x04, mmio::AccessSize::Word);
    EXPECT_NE(sr & 0x01u, 0u); // BSY = 1
    EXPECT_EQ(sr & 0x04u, 0u); // RXNE = 0

    // Colliding write after 8 ticks triggers OVR (bit 3 = 0x08)
    spi.tick(8);
    spi.write(0x08, 0x00, mmio::AccessSize::Byte);
    EXPECT_NE(spi.read(0x04, mmio::AccessSize::Word) & 0x08u, 0u); // OVR = 1

    // Finish remaining 8 ticks -> JEDEC ID 0xEF ready
    spi.tick(8);
    sr = spi.read(0x04, mmio::AccessSize::Word);
    EXPECT_EQ(sr & 0x01u, 0u); // BSY = 0
    EXPECT_NE(sr & 0x04u, 0u); // RXNE = 1
    EXPECT_EQ(spi.read(0x08, mmio::AccessSize::Byte), 0xEFu);
}

TEST(CustomDeviceTest, MultiCycleLatencyAndOverrun) {
    mmio::CustomDevice custom;
    custom.reset();

    custom.write(0x14, 5, mmio::AccessSize::Word); // LATENCY = 5 ticks
    custom.write(0x00, 6, mmio::AccessSize::Word); // A = 6
    custom.write(0x04, 7, mmio::AccessSize::Word); // B = 7
    custom.write(0x08, 2, mmio::AccessSize::Word); // CMD = MUL

    EXPECT_EQ(custom.read(0x10, mmio::AccessSize::Word) & 0x01u, 0u); // Not DONE
    EXPECT_NE(custom.read(0x10, mmio::AccessSize::Word) & 0x02u, 0u); // BUSY = 1
    EXPECT_EQ(custom.read(0x0C, mmio::AccessSize::Word), 0u);

    // Colliding CMD write at tick 3 triggers OVR (bit 2 = 0x04)
    custom.tick(3);
    custom.write(0x08, 1, mmio::AccessSize::Word);
    EXPECT_NE(custom.read(0x10, mmio::AccessSize::Word) & 0x04u, 0u);

    // Finish remaining 2 ticks -> 6 * 7 = 42
    custom.tick(2);
    EXPECT_NE(custom.read(0x10, mmio::AccessSize::Word) & 0x01u, 0u); // DONE = 1
    EXPECT_EQ(custom.read(0x0C, mmio::AccessSize::Word), 42u);
}

TEST(GpioTest, DirectionBsrrAndDebounceClockOverrun) {
    mmio::GpioDevice gpio;
    gpio.reset();

    // Configure pins [3:0] as output (0x0F), pins [7:4] as input
    gpio.write(0x00, 0x0Fu, mmio::AccessSize::Word);
    // Atomic BSRR: set bits 0 and 2 (0x05)
    gpio.write(0x0C, 0x05u, mmio::AccessSize::Word);
    EXPECT_EQ(gpio.read(0x08, mmio::AccessSize::Word), 0x05u);
    EXPECT_EQ(gpio.read(0x04, mmio::AccessSize::Word) & 0x0Fu, 0x05u);

    // Atomic BSRR: reset bit 0 (1 << 16) and set bit 1 (0x02) -> 0x06
    gpio.write(0x0C, (1u << 16) | 0x02u, mmio::AccessSize::Word);
    EXPECT_EQ(gpio.read(0x08, mmio::AccessSize::Word), 0x06u);

    // Enable interrupt on pin 4 (input button) and set DEBOUNCE = 4 ticks
    gpio.write(0x10, (1u << 4), mmio::AccessSize::Word);
    gpio.write(0x18, 4, mmio::AccessSize::Word);

    gpio.inject_external_input(1u << 4);
    EXPECT_FALSE(gpio.has_pending_irq()); // Still debouncing
    EXPECT_EQ(gpio.read(0x04, mmio::AccessSize::Word) & (1u << 4), 0u);

    // Bouncing input at tick 2 triggers OVR (bit 31)
    gpio.tick(2);
    gpio.inject_external_input(0);
    EXPECT_NE(gpio.read(0x14, mmio::AccessSize::Word) & (1u << 31), 0u);

    // Finish remaining 2 debounce ticks -> pin 4 settles high and fires IRQ
    gpio.tick(2);
    EXPECT_NE(gpio.read(0x04, mmio::AccessSize::Word) & (1u << 4), 0u);
    EXPECT_TRUE(gpio.has_pending_irq());

    // Clear ISR and OVR
    gpio.write(0x14, (1u << 31) | (1u << 4), mmio::AccessSize::Word);
    EXPECT_FALSE(gpio.has_pending_irq());
    EXPECT_EQ(gpio.read(0x14, mmio::AccessSize::Word), 0u);
}

TEST(DmaTest, FastBurstAndTurboClockedMemToMemAndMemToMMIO) {
    MemoryBus bus(1024 * 1024);
    auto dma_dev = std::make_unique<mmio::DmaDevice>(bus);
    auto gpio_dev = std::make_unique<mmio::GpioDevice>();
    mmio::DmaDevice* dma = dma_dev.get();
    mmio::GpioDevice* gpio = gpio_dev.get();
    bus.register_device(std::move(dma_dev));
    bus.register_device(std::move(gpio_dev));

    // Prepare 4 source words in RAM at 0x1000
    for (uint32_t i = 0; i < 4; ++i) {
        bus.write32(0x1000 + i * 4, 0xA0u + i);
    }

    // 1. Clocked Mem-to-Mem DMA: CLKDIV = 2 (2 ticks per word -> 8 ticks total)
    dma->write(0x14, 2, mmio::AccessSize::Word);      // CLKDIV = 2
    dma->write(0x00, 0x1000, mmio::AccessSize::Word); // SRC
    dma->write(0x04, 0x2000, mmio::AccessSize::Word); // DST
    dma->write(0x08, 4, mmio::AccessSize::Word);      // CNDTR = 4 words
    // CR: EN(1) | TCIE(2) | SRC_INC(4) | DST_INC(8) | SIZE_WORD(2 << 4 = 32) = 0x2F
    dma->write(0x0C, 0x2Fu, mmio::AccessSize::Word);

    EXPECT_NE(dma->read(0x10, mmio::AccessSize::Word) & 0x01u, 0u); // BUSY = 1
    EXPECT_FALSE(dma->has_pending_irq());

    // Colliding write to SRC after 4 ticks (2 words transferred) sets OVR (bit 3 = 0x08)
    bus.tick(4);
    EXPECT_EQ(dma->read(0x08, mmio::AccessSize::Word), 2u); // 2 words remaining
    dma->write(0x00, 0x3000, mmio::AccessSize::Word);
    EXPECT_NE(dma->read(0x10, mmio::AccessSize::Word) & 0x08u, 0u); // OVR = 1

    // Finish remaining 4 ticks -> all 4 words copied, TCIF & IRQ asserted
    bus.tick(4);
    EXPECT_EQ(dma->read(0x10, mmio::AccessSize::Word) & 0x01u, 0u); // BUSY = 0
    EXPECT_TRUE(dma->has_pending_irq());
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(bus.read32(0x2000 + i * 4), 0xA0u + i);
    }

    // Clear TCIF and OVR
    dma->write(0x10, 0x0Au, mmio::AccessSize::Word);
    EXPECT_FALSE(dma->has_pending_irq());

    // 2. Fast 0-cycle Mem-to-MMIO (GPIO ODR at 0x40005008, fixed DST)
    gpio->write(0x00, 0xFFu, mmio::AccessSize::Word); // MODER output
    dma->write(0x14, 0, mmio::AccessSize::Word);      // CLKDIV = 0 (0-cycle burst)
    dma->write(0x00, 0x100C, mmio::AccessSize::Word); // SRC (value 0xA3)
    dma->write(0x04, 0x40005008, mmio::AccessSize::Word); // DST = GPIO0->ODR
    dma->write(0x08, 1, mmio::AccessSize::Word);      // 1 word
    dma->write(0x0C, 0x25u, mmio::AccessSize::Word);  // EN | SRC_INC | SIZE_WORD
    EXPECT_EQ(gpio->read(0x08, mmio::AccessSize::Word), 0xA3u);
}

