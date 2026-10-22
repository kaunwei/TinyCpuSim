#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "tinyarmsim/mmio/device.hpp"

namespace tinyarmsim::mmio {

class NVICDevice : public MMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0xE000E000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;

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
