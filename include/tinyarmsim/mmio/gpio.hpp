#pragma once

#include <cstdint>
#include <string>
#include "tinyarmsim/mmio/device.hpp"

namespace tinyarmsim::mmio {

// 32-bit General-Purpose I/O Controller with Atomic BSRR, Edge Interrupts, and Input Debounce Clock
class GpioDevice : public MMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x40005000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;
    static constexpr uint32_t ISR_OVR_BIT  = (1u << 31); // Bit 31: Debounce Overrun Error

    explicit GpioDevice(uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN)
        : base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "GPIO0"; }
    [[nodiscard]] uint32_t irq_line() const noexcept override { return 2; }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) return moder_;
        if (offset == 0x04) {
            // IDR: combine output pins (from odr_) and input pins (from ext_idr_)
            return (odr_ & moder_) | (ext_idr_ & ~moder_);
        }
        if (offset == 0x08) return odr_;
        if (offset == 0x10) return ier_;
        if (offset == 0x14) {
            uint32_t st = isr_;
            if (ovr_) st |= ISR_OVR_BIT;
            return st;
        }
        if (offset == 0x18) return debounce_ticks_;
        if (offset == 0x1C) return (busy_ticks_ > 0) ? 1u : 0u; // Debounce BUSY status
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) {
            moder_ = value;
        } else if (offset == 0x08) {
            uint32_t old_pins = read(0x04, AccessSize::Word);
            odr_ = value;
            uint32_t new_pins = read(0x04, AccessSize::Word);
            uint32_t changed = (old_pins ^ new_pins) & 0x7FFFFFFFu;
            isr_ |= (changed & ier_);
        } else if (offset == 0x0C) {
            // BSRR: [15:0] atomic bit set, [31:16] atomic bit reset (reset has priority if both set)
            uint32_t old_pins = read(0x04, AccessSize::Word);
            uint32_t set_mask = value & 0xFFFFu;
            uint32_t rst_mask = (value >> 16) & 0xFFFFu;
            odr_ |= set_mask;
            odr_ &= ~rst_mask;
            uint32_t new_pins = read(0x04, AccessSize::Word);
            uint32_t changed = (old_pins ^ new_pins) & 0x7FFFFFFFu;
            isr_ |= (changed & ier_);
        } else if (offset == 0x10) {
            ier_ = value & 0x7FFFFFFFu;
        } else if (offset == 0x14) {
            // Write-1-to-clear ISR and OVR
            if (value & ISR_OVR_BIT) ovr_ = false;
            isr_ &= ~(value & 0x7FFFFFFFu);
        } else if (offset == 0x18) {
            debounce_ticks_ = value;
        }
    }

    // External pin signal injection (simulates button press / sensor input on input pins)
    void inject_external_input(uint32_t pin_values) {
        if (busy_ticks_ > 0) {
            ovr_ = true; // Bouncing/changing input faster than debounce clock window
            return;
        }
        if (debounce_ticks_ == 0) {
            commit_external_input(pin_values);
        } else {
            pending_ext_idr_ = pin_values;
            busy_ticks_ = debounce_ticks_;
        }
    }

    void tick(uint64_t cycles) override {
        if (busy_ticks_ > 0) {
            if (cycles >= busy_ticks_) {
                busy_ticks_ = 0;
                commit_external_input(pending_ext_idr_);
            } else {
                busy_ticks_ -= cycles;
            }
        }
    }

    void reset() override {
        moder_ = 0;
        odr_ = 0;
        ext_idr_ = 0;
        pending_ext_idr_ = 0;
        ier_ = 0;
        isr_ = 0;
        debounce_ticks_ = 0;
        busy_ticks_ = 0;
        ovr_ = false;
    }

    [[nodiscard]] bool has_pending_irq() const noexcept override {
        return (isr_ & ier_) != 0;
    }

private:
    void commit_external_input(uint32_t pin_values) {
        uint32_t old_pins = read(0x04, AccessSize::Word);
        ext_idr_ = pin_values;
        uint32_t new_pins = read(0x04, AccessSize::Word);
        uint32_t changed = (old_pins ^ new_pins) & 0x7FFFFFFFu;
        isr_ |= (changed & ier_);
    }

    uint32_t base_;
    uint32_t span_;
    uint32_t moder_{0};
    uint32_t odr_{0};
    uint32_t ext_idr_{0};
    uint32_t pending_ext_idr_{0};
    uint32_t ier_{0};
    uint32_t isr_{0};
    uint32_t debounce_ticks_{0};
    uint64_t busy_ticks_{0};
    bool ovr_{false};
};

} // namespace tinyarmsim::mmio
