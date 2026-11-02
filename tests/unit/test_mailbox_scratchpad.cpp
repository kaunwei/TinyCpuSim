#include <gtest/gtest.h>
#include "platforms/03_coprocessor_accel/devices/mailbox_scratchpad.h"
#include "platforms/03_coprocessor_accel/devices/mailbox_scratchpad.hpp"
#include "tinyarmsim/memory_bus.hpp"

using namespace tinyarmsim;
using namespace tinyarmsim::mmio;

TEST(MailboxScratchpadTest, DefaultInitializationAndAttributes) {
    MailboxScratchpadDevice dev;
    EXPECT_EQ(dev.base_address(), MailboxScratchpadDevice::DEFAULT_BASE);
    EXPECT_EQ(dev.address_span(), MailboxScratchpadDevice::DEFAULT_SPAN);
    EXPECT_EQ(dev.device_name(), "MailboxScratchpad");
    EXPECT_EQ(dev.irq_line(), MailboxScratchpadDevice::DEFAULT_HOST_IRQ_LINE);
    EXPECT_FALSE(dev.has_pending_irq());
    EXPECT_FALSE(dev.doorbell_to_accel_pending());
    EXPECT_FALSE(dev.doorbell_to_host_pending());
    EXPECT_EQ(dev.scratchpad_size(), 64 * 1024u);

    // Initial register reads
    EXPECT_EQ(dev.read(MB_REG_STATUS, AccessSize::Word), MB_STATUS_IDLE);
    EXPECT_EQ(dev.read(MB_REG_PARAM_WIDTH, AccessSize::Word), 0u);
    EXPECT_EQ(dev.read(MB_REG_PARAM_HEIGHT, AccessSize::Word), 0u);
    EXPECT_EQ(dev.read(MB_REG_PARAM_CHANNELS, AccessSize::Word), 0u);
    EXPECT_EQ(dev.read(MB_REG_PARAM_KERNEL_SZ, AccessSize::Word), 0u);
    EXPECT_EQ(dev.read(MB_REG_DOORBELL_TO_ACCEL, AccessSize::Word), 0u);
    EXPECT_EQ(dev.read(MB_REG_DOORBELL_TO_HOST, AccessSize::Word), 0u);
    EXPECT_EQ(dev.read(MB_REG_INTCLR, AccessSize::Word), 0u);
}

TEST(MailboxScratchpadTest, RegisterReadWrite) {
    MailboxScratchpadDevice dev;

    // Status register
    dev.write(MB_REG_STATUS, MB_STATUS_RUNNING, AccessSize::Word);
    EXPECT_EQ(dev.read(MB_REG_STATUS, AccessSize::Word), MB_STATUS_RUNNING);

    dev.write(MB_REG_STATUS, MB_STATUS_DONE, AccessSize::Word);
    EXPECT_EQ(dev.read(MB_REG_STATUS, AccessSize::Word), MB_STATUS_DONE);

    dev.write(MB_REG_STATUS, MB_STATUS_ERROR, AccessSize::Word);
    EXPECT_EQ(dev.read(MB_REG_STATUS, AccessSize::Word), MB_STATUS_ERROR);

    // Parameter registers
    dev.write(MB_REG_PARAM_WIDTH, 128, AccessSize::Word);
    EXPECT_EQ(dev.read(MB_REG_PARAM_WIDTH, AccessSize::Word), 128u);

    dev.write(MB_REG_PARAM_HEIGHT, 64, AccessSize::Word);
    EXPECT_EQ(dev.read(MB_REG_PARAM_HEIGHT, AccessSize::Word), 64u);

    dev.write(MB_REG_PARAM_CHANNELS, 3, AccessSize::Word);
    EXPECT_EQ(dev.read(MB_REG_PARAM_CHANNELS, AccessSize::Word), 3u);

    dev.write(MB_REG_PARAM_KERNEL_SZ, 5, AccessSize::Word);
    EXPECT_EQ(dev.read(MB_REG_PARAM_KERNEL_SZ, AccessSize::Word), 5u);
}

TEST(MailboxScratchpadTest, DoorbellToAccelAndCallback) {
    MailboxScratchpadDevice dev;
    bool callback_called = false;
    dev.set_accel_irq_callback([&callback_called]() {
        callback_called = true;
    });

    EXPECT_FALSE(dev.doorbell_to_accel_pending());
    EXPECT_FALSE(callback_called);

    // Ring DOORBELL_TO_ACCEL
    dev.write(MB_REG_DOORBELL_TO_ACCEL, 1, AccessSize::Word);
    EXPECT_TRUE(dev.doorbell_to_accel_pending());
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(dev.read(MB_REG_DOORBELL_TO_ACCEL, AccessSize::Word), 1u);

    // Clear via INTCLR
    dev.write(MB_REG_INTCLR, 0x02, AccessSize::Word); // Clear accel doorbell
    EXPECT_FALSE(dev.doorbell_to_accel_pending());
    EXPECT_EQ(dev.read(MB_REG_DOORBELL_TO_ACCEL, AccessSize::Word), 0u);

    // Ring again via method
    callback_called = false;
    dev.signal_accel_irq();
    EXPECT_TRUE(dev.doorbell_to_accel_pending());
    EXPECT_TRUE(callback_called);

    // Direct clear
    dev.clear_accel_doorbell();
    EXPECT_FALSE(dev.doorbell_to_accel_pending());
}

TEST(MailboxScratchpadTest, DoorbellToHostAndInterrupt) {
    MailboxScratchpadDevice dev;
    EXPECT_FALSE(dev.has_pending_irq());
    EXPECT_FALSE(dev.doorbell_to_host_pending());

    // Write DOORBELL_TO_HOST
    dev.write(MB_REG_DOORBELL_TO_HOST, 1, AccessSize::Word);
    EXPECT_TRUE(dev.doorbell_to_host_pending());
    EXPECT_TRUE(dev.has_pending_irq());
    EXPECT_EQ(dev.read(MB_REG_DOORBELL_TO_HOST, AccessSize::Word), 1u);

    // Clear via INTCLR bit 0
    dev.write(MB_REG_INTCLR, 0x01, AccessSize::Word);
    EXPECT_FALSE(dev.doorbell_to_host_pending());
    EXPECT_FALSE(dev.has_pending_irq());
    EXPECT_EQ(dev.read(MB_REG_DOORBELL_TO_HOST, AccessSize::Word), 0u);

    // Trigger host doorbell and test INTCLR with both bits
    dev.write(MB_REG_DOORBELL_TO_HOST, 1, AccessSize::Word);
    dev.write(MB_REG_DOORBELL_TO_ACCEL, 1, AccessSize::Word);
    EXPECT_TRUE(dev.has_pending_irq());
    EXPECT_TRUE(dev.doorbell_to_accel_pending());

    dev.write(MB_REG_INTCLR, 0x03, AccessSize::Word);
    EXPECT_FALSE(dev.has_pending_irq());
    EXPECT_FALSE(dev.doorbell_to_accel_pending());
}

TEST(MailboxScratchpadTest, ScratchpadAccessByteHalfWordWord) {
    MailboxScratchpadDevice dev;

    // Byte writes and reads
    dev.write(MB_SCRATCHPAD_OFFSET + 0, 0x11, AccessSize::Byte);
    dev.write(MB_SCRATCHPAD_OFFSET + 1, 0x22, AccessSize::Byte);
    dev.write(MB_SCRATCHPAD_OFFSET + 2, 0x33, AccessSize::Byte);
    dev.write(MB_SCRATCHPAD_OFFSET + 3, 0x44, AccessSize::Byte);

    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 0, AccessSize::Byte), 0x11u);
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 1, AccessSize::Byte), 0x22u);
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 2, AccessSize::Byte), 0x33u);
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 3, AccessSize::Byte), 0x44u);

    // Word read of the 4 bytes in little-endian order
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 0, AccessSize::Word), 0x44332211u);

    // Halfword read
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 0, AccessSize::HalfWord), 0x2211u);
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 2, AccessSize::HalfWord), 0x4433u);

    // Halfword write
    dev.write(MB_SCRATCHPAD_OFFSET + 4, 0xABCD, AccessSize::HalfWord);
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 4, AccessSize::HalfWord), 0xABCDu);
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 4, AccessSize::Byte), 0xCDu);
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 5, AccessSize::Byte), 0xABu);

    // Word write
    dev.write(MB_SCRATCHPAD_OFFSET + 8, 0x12345678, AccessSize::Word);
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 8, AccessSize::Word), 0x12345678u);
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 8, AccessSize::Byte), 0x78u);
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 9, AccessSize::Byte), 0x56u);
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 10, AccessSize::Byte), 0x34u);
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 11, AccessSize::Byte), 0x12u);
}

TEST(MailboxScratchpadTest, ScratchpadBoundaryAndDirectAccess) {
    MailboxScratchpadDevice dev;
    constexpr uint32_t last_word_offset = MB_SCRATCHPAD_OFFSET + MB_SCRATCHPAD_SIZE - 4;

    dev.write(last_word_offset, 0xDEADBEEF, AccessSize::Word);
    EXPECT_EQ(dev.read(last_word_offset, AccessSize::Word), 0xDEADBEEFu);

    // Check direct pointer access
    uint8_t* raw = dev.scratchpad_data();
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw[MB_SCRATCHPAD_SIZE - 4], 0xEF);
    EXPECT_EQ(raw[MB_SCRATCHPAD_SIZE - 3], 0xBE);
    EXPECT_EQ(raw[MB_SCRATCHPAD_SIZE - 2], 0xAD);
    EXPECT_EQ(raw[MB_SCRATCHPAD_SIZE - 1], 0xDE);

    // Read beyond boundary returns 0
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + MB_SCRATCHPAD_SIZE, AccessSize::Word), 0u);
}

TEST(MailboxScratchpadTest, ResetClearsStateAndMemory) {
    MailboxScratchpadDevice dev;
    dev.write(MB_REG_STATUS, MB_STATUS_DONE, AccessSize::Word);
    dev.write(MB_REG_PARAM_WIDTH, 64, AccessSize::Word);
    dev.write(MB_REG_DOORBELL_TO_HOST, 1, AccessSize::Word);
    dev.write(MB_SCRATCHPAD_OFFSET + 0, 0xA5A5A5A5, AccessSize::Word);

    EXPECT_EQ(dev.read(MB_REG_STATUS, AccessSize::Word), MB_STATUS_DONE);
    EXPECT_TRUE(dev.has_pending_irq());

    dev.reset();

    EXPECT_EQ(dev.read(MB_REG_STATUS, AccessSize::Word), MB_STATUS_IDLE);
    EXPECT_EQ(dev.read(MB_REG_PARAM_WIDTH, AccessSize::Word), 0u);
    EXPECT_FALSE(dev.has_pending_irq());
    EXPECT_FALSE(dev.doorbell_to_host_pending());
    EXPECT_FALSE(dev.doorbell_to_accel_pending());
    EXPECT_EQ(dev.read(MB_SCRATCHPAD_OFFSET + 0, AccessSize::Word), 0u);
}

TEST(MailboxScratchpadTest, IntegrationWithMemoryBus) {
    MemoryBus bus;
    auto dev = std::make_unique<MailboxScratchpadDevice>(0x50000000, 0x20000);
    bus.register_device(0x50000000, 0x20000, std::move(dev));

    // Write parameters via bus
    bus.write32(0x50000000 + MB_REG_PARAM_WIDTH, 32);
    bus.write32(0x50000000 + MB_REG_PARAM_HEIGHT, 32);
    EXPECT_EQ(bus.read32(0x50000000 + MB_REG_PARAM_WIDTH), 32u);
    EXPECT_EQ(bus.read32(0x50000000 + MB_REG_PARAM_HEIGHT), 32u);

    // Scratchpad write via bus
    bus.write32(0x50000000 + MB_SCRATCHPAD_OFFSET, 0xCAFEFACE);
    EXPECT_EQ(bus.read32(0x50000000 + MB_SCRATCHPAD_OFFSET), 0xCAFEFACEu);
}
