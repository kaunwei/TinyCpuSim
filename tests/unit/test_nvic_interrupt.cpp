#include <gtest/gtest.h>
#include <memory>
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/mmio/device.hpp"
#include "tinyarmsim/mmio/nvic.hpp"

using namespace tinyarmsim;

TEST(NVICTest, RegisterReadWrite) {
    mmio::NVICDevice nvic;
    EXPECT_EQ(nvic.read(0x100, mmio::AccessSize::Word), 0u);

    // Enable IRQ 1 and 3
    nvic.write(0x100, 0x0A, mmio::AccessSize::Word);
    EXPECT_EQ(nvic.read(0x100, mmio::AccessSize::Word), 0x0Au);
    EXPECT_TRUE(nvic.is_enabled(1));
    EXPECT_TRUE(nvic.is_enabled(3));
    EXPECT_FALSE(nvic.is_enabled(0));

    // Disable IRQ 1 via ICER
    nvic.write(0x180, 0x02, mmio::AccessSize::Word);
    EXPECT_EQ(nvic.read(0x100, mmio::AccessSize::Word), 0x08u);

    // Pending set & clear
    nvic.write(0x200, 0x08, mmio::AccessSize::Word);
    EXPECT_TRUE(nvic.is_pending(3));
    EXPECT_EQ(nvic.get_highest_pending_irq(), 3);

    nvic.write(0x280, 0x08, mmio::AccessSize::Word);
    EXPECT_FALSE(nvic.is_pending(3));
    EXPECT_EQ(nvic.get_highest_pending_irq(), -1);

    // VTOR and IPR read/write
    nvic.write(0xD08, 0x20000000, mmio::AccessSize::Word);
    EXPECT_EQ(nvic.read(0xD08, mmio::AccessSize::Word), 0x20000000u);
    EXPECT_EQ(nvic.vtor(), 0x20000000u);

    nvic.write(0x400, 0x10203040, mmio::AccessSize::Word);
    EXPECT_EQ(nvic.read(0x400, mmio::AccessSize::Word), 0x10203040u);

    // Reset clears registers
    nvic.reset();
    EXPECT_EQ(nvic.read(0x100, mmio::AccessSize::Word), 0u);
    EXPECT_EQ(nvic.read(0x200, mmio::AccessSize::Word), 0u);
    EXPECT_EQ(nvic.read(0x400, mmio::AccessSize::Word), 0u);
    EXPECT_EQ(nvic.read(0xD08, mmio::AccessSize::Word), 0u);
}

TEST(NVICTest, AutoStackingAndExcReturn) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    IsaInterpreter interp(state, bus);

    auto nvic_dev = std::make_unique<mmio::NVICDevice>();
    mmio::NVICDevice* nvic = nvic_dev.get();
    bus.register_device(std::move(nvic_dev));
    interp.attach_nvic(nvic);

    // Set vector table at 0x1000
    nvic->set_vtor(0x1000);
    // Vector for IRQ 0 is index 16 (offset 16*4 = 64 = 0x40 -> 0x1040)
    uint32_t isr_pc = 0x2000;
    bus.write32(0x1040, isr_pc | 1); // Thumb bit set

    // ISR code at 0x2000: BX LR (0x4770)
    bus.write16(0x2000, 0x4770);

    // Main thread setup
    uint32_t initial_sp = 0x10000;
    uint32_t main_pc = 0x0100;
    state.set_reg(13, initial_sp);
    state.set_pc(main_pc);
    state.set_reg(0, 0x1111);
    state.set_reg(1, 0x2222);
    state.set_reg(2, 0x3333);
    state.set_reg(3, 0x4444);
    state.set_reg(12, 0xAAAA);
    state.set_reg(14, 0xBBBB);

    // Enable and trigger IRQ 0
    nvic->write(0x100, 1, mmio::AccessSize::Word); // Enable IRQ 0
    nvic->set_pending(0);

    // Step 1: Exception entry & auto-stacking
    interp.step();
    EXPECT_EQ(state.get_pc(), isr_pc);
    EXPECT_EQ(state.get_reg(14), 0xFFFFFFF9u);
    EXPECT_EQ(state.get_reg(13), initial_sp - 32);

    // Verify stacked values
    uint32_t sp = state.get_reg(13);
    EXPECT_EQ(bus.read32(sp), 0x1111u);
    EXPECT_EQ(bus.read32(sp + 4), 0x2222u);
    EXPECT_EQ(bus.read32(sp + 8), 0x3333u);
    EXPECT_EQ(bus.read32(sp + 12), 0x4444u);
    EXPECT_EQ(bus.read32(sp + 16), 0xAAAAu);
    EXPECT_EQ(bus.read32(sp + 20), 0xBBBBu);
    EXPECT_EQ(bus.read32(sp + 24), main_pc);

    // Step 2: Execute BX LR in ISR -> Exception return unstacking
    interp.step();
    EXPECT_EQ(state.get_pc(), main_pc);
    EXPECT_EQ(state.get_reg(13), initial_sp);
    EXPECT_EQ(state.get_reg(0), 0x1111u);
    EXPECT_EQ(state.get_reg(1), 0x2222u);
    EXPECT_EQ(state.get_reg(14), 0xBBBBu);
}

class MockInterruptDevice : public mmio::MMIODevice {
public:
    MockInterruptDevice(uint32_t base, uint32_t span, uint32_t irq)
        : base_(base), span_(span), irq_(irq) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "MockInterruptDevice"; }

    [[nodiscard]] uint32_t read([[maybe_unused]] uint32_t offset, [[maybe_unused]] mmio::AccessSize size) override {
        return 0;
    }

    void write([[maybe_unused]] uint32_t offset, [[maybe_unused]] uint32_t value, [[maybe_unused]] mmio::AccessSize size) override {}

    [[nodiscard]] bool has_pending_irq() const noexcept override { return pending_; }
    [[nodiscard]] uint32_t irq_line() const noexcept override { return irq_; }

    void trigger() noexcept { pending_ = true; }
    void clear() noexcept { pending_ = false; }

private:
    uint32_t base_;
    uint32_t span_;
    uint32_t irq_;
    bool pending_{false};
};

TEST(NVICTest, DeviceIrqPollingAndArbitration) {
    MemoryBus bus(64 * 1024 * 1024);
    ArchitecturalState state;
    IsaInterpreter interp(state, bus);

    auto nvic_dev = std::make_unique<mmio::NVICDevice>();
    mmio::NVICDevice* nvic = nvic_dev.get();
    bus.register_device(std::move(nvic_dev));
    interp.attach_nvic(nvic);

    auto mock_dev = std::make_unique<MockInterruptDevice>(0x40000000, 0x1000, 2);
    MockInterruptDevice* mock = mock_dev.get();
    bus.register_device(std::move(mock_dev));

    nvic->write(0x100, (1u << 2), mmio::AccessSize::Word); // Enable IRQ 2
    nvic->set_vtor(0x0);

    // Vector for IRQ 2 is (16 + 2) * 4 = 72 = 0x48
    uint32_t isr_pc = 0x3000;
    bus.write32(0x48, isr_pc | 1);
    bus.write16(0x3000, 0x4770); // BX LR

    state.set_reg(13, 0x8000);
    state.set_pc(0x0200);

    // Trigger mock device IRQ
    mock->trigger();

    // Step into ISR
    interp.step();
    EXPECT_EQ(state.get_pc(), isr_pc);
    EXPECT_EQ(state.get_reg(14), 0xFFFFFFF9u);
    EXPECT_EQ(state.get_reg(13), 0x8000u - 32);

    // Peripheral interrupt deasserted in ISR
    mock->clear();

    // Return from ISR
    interp.step();
    EXPECT_EQ(state.get_pc(), 0x0200u);
    EXPECT_EQ(state.get_reg(13), 0x8000u);
}
