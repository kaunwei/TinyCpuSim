#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "tinyarmsim/mmio/device.hpp"
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/memory_bus.hpp"

namespace tinyarmsim::mmio {

class NVICDevice : public MMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0xE000E000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;
    static constexpr uint32_t EXC_RETURN_THREAD_MSP = 0xFFFFFFF9u;

    explicit NVICDevice(uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN)
        : base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "NVIC"; }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        // Offset 0x100: ISER0
        if (offset == 0x100) return iser_;
        // Offset 0x180: ICER0
        if (offset == 0x180) return iser_;
        // Offset 0x200: ISPR0
        if (offset == 0x200) return ispr_;
        // Offset 0x280: ICPR0
        if (offset == 0x280) return ispr_;
        // Offset 0x300: IABR0
        if (offset == 0x300) return iabr_;
        // Offset 0x400 - 0x41C: IPR0 - IPR7
        if (offset >= 0x400 && offset <= 0x41C) {
            size_t idx = (offset - 0x400) / 4;
            return ipr_[idx];
        }
        // Offset 0xD08: VTOR
        if (offset == 0xD08) return vtor_;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x100) {
            iser_ |= value; // Write 1 to enable
        } else if (offset == 0x180) {
            iser_ &= ~value; // Write 1 to disable
        } else if (offset == 0x200) {
            ispr_ |= value; // Write 1 to set pending
        } else if (offset == 0x280) {
            ispr_ &= ~value; // Write 1 to clear pending
        } else if (offset >= 0x400 && offset <= 0x41C) {
            size_t idx = (offset - 0x400) / 4;
            ipr_[idx] = value;
        } else if (offset == 0xD08) {
            vtor_ = value;
        }
    }

    void reset() override {
        iser_ = 0;
        ispr_ = 0;
        iabr_ = 0;
        for (auto& p : ipr_) p = 0;
        vtor_ = 0;
    }

    void set_pending(uint32_t irq) {
        if (irq < 32) ispr_ |= (1u << irq);
    }

    void clear_pending(uint32_t irq) {
        if (irq < 32) ispr_ &= ~(1u << irq);
    }

    [[nodiscard]] bool is_enabled(uint32_t irq) const noexcept {
        return (irq < 32) && ((iser_ & (1u << irq)) != 0);
    }

    [[nodiscard]] bool is_pending(uint32_t irq) const noexcept {
        return (irq < 32) && ((ispr_ & (1u << irq)) != 0);
    }

    [[nodiscard]] int get_highest_pending_irq() const noexcept {
        uint32_t active_pending = iser_ & ispr_;
        if (active_pending == 0) return -1;
        // Priority arbitration: lowest IRQ number wins
        for (uint32_t i = 0; i < 32; ++i) {
            if (active_pending & (1u << i)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void acknowledge_irq(uint32_t irq) noexcept {
        if (irq < 32) {
            ispr_ &= ~(1u << irq);
            iabr_ |= (1u << irq);
        }
    }

    void complete_active_irq() noexcept {
        // Complete lowest active IRQ
        for (uint32_t i = 0; i < 32; ++i) {
            if (iabr_ & (1u << i)) {
                iabr_ &= ~(1u << i);
                break;
            }
        }
    }

    // Deep seam: polls all registered MMIO devices for IRQ lines, arbitrates against IABR,
    // and performs Cortex-M 8-word hardware auto-stacking if a higher-priority IRQ is ready.
    bool poll_and_try_enter_exception(ArchitecturalState& state, MemoryBus& bus) {
        for (const auto& slot : bus.mmio_devices()) {
            if (slot.dev && slot.dev.get() != this && slot.dev->has_pending_irq()) {
                set_pending(slot.dev->irq_line());
            }
        }
        int irq = get_highest_pending_irq();
        if (irq < 0) return false;

        // Check if an equal or higher-priority (lower index) IRQ is already active in IABR
        for (uint32_t a = 0; a <= static_cast<uint32_t>(irq); ++a) {
            if (iabr_ & (1u << a)) {
                return false;
            }
        }

        // Cortex-M hardware auto-stacking onto MSP (R13)
        uint32_t sp = state.get_reg(13) - 32u;
        bus.write32(sp,      state.get_reg(0));
        bus.write32(sp + 4,  state.get_reg(1));
        bus.write32(sp + 8,  state.get_reg(2));
        bus.write32(sp + 12, state.get_reg(3));
        bus.write32(sp + 16, state.get_reg(12));
        bus.write32(sp + 20, state.get_reg(14)); // LR
        bus.write32(sp + 24, state.get_pc());     // Return PC
        bus.write32(sp + 28, state.get_cpsr());   // xPSR
        state.set_reg(13, sp);
        state.set_reg(14, EXC_RETURN_THREAD_MSP);

        uint32_t vector_addr = vtor_ + (16u + static_cast<uint32_t>(irq)) * 4u;
        uint32_t isr_addr = bus.read32(vector_addr) & ~1u;
        state.set_pc(isr_addr);
        acknowledge_irq(static_cast<uint32_t>(irq));
        return true;
    }

    // Deep seam: checks if PC holds a Cortex-M EXC_RETURN magic value (0xFFFFFFF0 mask)
    // and unstacks the 8-word exception frame back into ArchitecturalState.
    static bool try_exit_exception(ArchitecturalState& state, MemoryBus& bus, NVICDevice* nvic) {
        if ((state.get_pc() & 0xFFFFFFF0u) != 0xFFFFFFF0u) {
            return false;
        }
        uint32_t sp = state.get_reg(13);
        state.set_reg(0,  bus.read32(sp));
        state.set_reg(1,  bus.read32(sp + 4));
        state.set_reg(2,  bus.read32(sp + 8));
        state.set_reg(3,  bus.read32(sp + 12));
        state.set_reg(12, bus.read32(sp + 16));
        state.set_reg(14, bus.read32(sp + 20));
        uint32_t return_pc = bus.read32(sp + 24) & ~1u;
        state.set_cpsr(bus.read32(sp + 28));
        state.set_reg(13, sp + 32u);
        state.set_pc(return_pc);
        if (nvic) nvic->complete_active_irq();
        return true;
    }

    [[nodiscard]] uint32_t vtor() const noexcept { return vtor_; }
    void set_vtor(uint32_t vtor) noexcept { vtor_ = vtor; }

private:
    uint32_t base_;
    uint32_t span_;
    uint32_t iser_{0};
    uint32_t ispr_{0};
    uint32_t iabr_{0};
    uint32_t ipr_[8]{0};
    uint32_t vtor_{0};
};

} // namespace tinyarmsim::mmio
