#include <gtest/gtest.h>
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/mmio/device.hpp"

using namespace tinyarmsim;

class DummyDevice : public mmio::MMIODevice {
public:
    explicit DummyDevice(uint32_t base = 0x40000000, uint32_t span = 0x1000)
        : base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }

    [[nodiscard]] uint32_t read(uint32_t offset, mmio::AccessSize size) override {
        last_offset_ = offset;
        last_size_ = size;
        if (offset == 0) return reg0_;
        if (offset == 4) return reg1_;
        return 0xCAFEBABE;
    }

    void write(uint32_t offset, uint32_t value, mmio::AccessSize size) override {
        last_offset_ = offset;
        last_size_ = size;
        if (offset == 0) reg0_ = value;
        if (offset == 4) reg1_ = value;
    }

    void tick(uint64_t cycles) override {
        tick_count_ += cycles;
    }

    void reset() override {
        reg0_ = 0;
        reg1_ = 0;
        tick_count_ = 0;
    }

    [[nodiscard]] bool has_pending_irq() const noexcept override { return pending_irq_; }
    [[nodiscard]] uint32_t irq_line() const noexcept override { return 7; }

    uint32_t reg0_{0x11223344};
    uint32_t reg1_{0x55667788};
    uint64_t tick_count_{0};
    bool pending_irq_{false};
    uint32_t last_offset_{0};
    mmio::AccessSize last_size_{mmio::AccessSize::Word};

private:
    uint32_t base_;
    uint32_t span_;
};

TEST(MMIOBusTest, FlatMemoryBackwardsCompatibility) {
    MemoryBus bus(1024 * 1024);
    bus.write32(0x100, 0x12345678);
    EXPECT_EQ(bus.read32(0x100), 0x12345678u);
    EXPECT_EQ(bus.read16(0x100), 0x5678u);
    EXPECT_EQ(bus.read8(0x100), 0x78u);
}

TEST(MMIOBusTest, DeviceRegistrationAndAccess) {
    MemoryBus bus(1024 * 1024);
    auto dev = std::make_unique<DummyDevice>(0x40000000, 0x1000);
    DummyDevice* dev_ptr = dev.get();
    bus.register_device(std::move(dev));

    EXPECT_EQ(bus.get_device_at(0x40000000), dev_ptr);
    EXPECT_EQ(bus.get_device_at<DummyDevice>(0x40000004), dev_ptr);
    EXPECT_EQ(bus.get_device_at(0x50000000), nullptr);

    EXPECT_EQ(bus.read32(0x40000000), 0x11223344u);
    EXPECT_EQ(dev_ptr->last_size_, mmio::AccessSize::Word);

    bus.write32(0x40000004, 0xAABBCCDD);
    EXPECT_EQ(dev_ptr->reg1_, 0xAABBCCDDu);
    EXPECT_EQ(bus.read32(0x40000004), 0xAABBCCDDu);

    // Byte & HalfWord access
    bus.write8(0x40000000, 0x55);
    EXPECT_EQ(dev_ptr->last_size_, mmio::AccessSize::Byte);
    EXPECT_EQ(dev_ptr->reg0_, 0x55u);

    bus.write16(0x40000000, 0x1234);
    EXPECT_EQ(dev_ptr->last_size_, mmio::AccessSize::HalfWord);
    EXPECT_EQ(dev_ptr->reg0_, 0x1234u);
}

TEST(MMIOBusTest, OverlapRegistrationThrows) {
    MemoryBus bus(1024 * 1024);
    bus.register_device(std::make_unique<DummyDevice>(0x40000000, 0x1000));
    EXPECT_THROW(bus.register_device(std::make_unique<DummyDevice>(0x40000800, 0x1000)), std::invalid_argument);
    EXPECT_THROW(bus.register_device(nullptr), std::invalid_argument);
}

TEST(MMIOBusTest, TickAndResetPropagation) {
    MemoryBus bus(1024 * 1024);
    auto dev = std::make_unique<DummyDevice>(0x40000000, 0x1000);
    DummyDevice* dev_ptr = dev.get();
    bus.register_device(std::move(dev));

    bus.tick(42);
    EXPECT_EQ(dev_ptr->tick_count_, 42u);

    bus.write32(0x40000000, 0xDEADBEEF);
    bus.reset();
    EXPECT_EQ(dev_ptr->reg0_, 0u);
    EXPECT_EQ(dev_ptr->tick_count_, 0u);
}
