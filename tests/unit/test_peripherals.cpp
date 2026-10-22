#include <gtest/gtest.h>
#include "tinyarmsim/mmio/i2c.hpp"
#include "tinyarmsim/mmio/spi.hpp"
#include "tinyarmsim/mmio/crc32.hpp"
#include "tinyarmsim/mmio/custom_device.hpp"

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
