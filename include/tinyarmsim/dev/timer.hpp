#pragma once

#include <cstdint>
#include <string>
#include "tinyarmsim/dev/device.hpp"

namespace tinyarmsim::mmio {

class TimerDevice : public MMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x40001000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;

    explicit TimerDevice(uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN)
        : base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "Timer0"; }
    [[nodiscard]] uint32_t irq_line() const noexcept override { return 1; }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) return load_;
        if (offset == 0x04) return value_;
        if (offset == 0x08) return control_;
        if (offset == 0x10) return raw_irq_ ? 1u : 0u; // RIS
        if (offset == 0x14) return (raw_irq_ && ((control_ & 0x04u) != 0)) ? 1u : 0u; // MIS
        if (offset == 0x18) return psc_; // Prescaler
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) {
            load_ = value;
            value_ = value;
            psc_counter_ = 0;
        } else if (offset == 0x04) {
            value_ = value;
            psc_counter_ = 0;
        } else if (offset == 0x08) {
            control_ = value;
        } else if (offset == 0x0C) {
            raw_irq_ = false; // INTCLR
        } else if (offset == 0x18) {
            psc_ = value;
            psc_counter_ = 0;
        }
    }

    void tick(uint64_t cycles) override {
        if ((control_ & 0x01u) == 0) return; // Not enabled
        if (load_ == 0) return;

        uint64_t step_ticks = static_cast<uint64_t>(psc_) + 1ULL;
        psc_counter_ += cycles;
        uint64_t decrements = psc_counter_ / step_ticks;
        if (decrements == 0) return;
        psc_counter_ %= step_ticks;

        if (decrements >= value_) {
            raw_irq_ = true;
            if ((control_ & 0x02u) != 0) { // Periodic auto-reload
                uint64_t rem = (decrements - value_) % load_;
                value_ = static_cast<uint32_t>(load_ - rem);
            } else {
                value_ = 0;
                control_ &= ~0x01u; // One-shot disables timer
            }
        } else {
            value_ -= static_cast<uint32_t>(decrements);
        }
    }

    void reset() override {
        load_ = 0;
        value_ = 0;
        control_ = 0;
        psc_ = 0;
        psc_counter_ = 0;
        raw_irq_ = false;
    }

    [[nodiscard]] bool has_pending_irq() const noexcept override {
        return raw_irq_ && ((control_ & 0x04u) != 0);
    }

private:
    uint32_t base_;
    uint32_t span_;
    uint32_t load_{0};
    uint32_t value_{0};
    uint32_t control_{0}; // Bit 0: Enable, Bit 1: Periodic, Bit 2: IRQ Enable
    uint32_t psc_{0};     // Prescaler: divides bus clock by (psc_ + 1)
    uint64_t psc_counter_{0};
    bool raw_irq_{false};
};

} // namespace tinyarmsim::mmio
