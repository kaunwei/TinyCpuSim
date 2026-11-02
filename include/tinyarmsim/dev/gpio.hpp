#pragma once

#include <cstdint>
#include <string>
#include "tinyarmsim/dev/device.hpp"

namespace tinyarmsim::mmio {

class GpioDevice : public ClockedMMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x40005000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;
    static constexpr uint32_t ISR_OVR_BIT  = (1u << 31);

    explicit GpioDevice(uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN)
        : base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "GPIO0"; }
    [[nodiscard]] uint32_t irq_line() const noexcept override { return 2; }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) return moder_;
        if (offset == 0x04) return (odr_ & moder_) | (ext_idr_ & ~moder_);
        if (offset == 0x08) return odr_;
        if (offset == 0x10) return ier_;
        if (offset == 0x14) {
            uint32_t st = isr_;
            if (has_overrun()) st |= ISR_OVR_BIT;
            return st;
        }
        if (offset == 0x18) return debounce_ticks_;
        if (offset == 0x1C) return is_busy() ? 1u : 0u;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) {
            moder_ = value;
        } else if (offset == 0x08) {
            update_pins([&]() { odr_ = value; });
        } else if (offset == 0x0C) {
            update_pins([&]() {
                uint32_t set_mask = value & 0xFFFFu;
                uint32_t rst_mask = (value >> 16) & 0xFFFFu;
                odr_ |= set_mask;
                odr_ &= ~rst_mask;
            });
        } else if (offset == 0x10) {
            ier_ = value & 0x7FFFFFFFu;
        } else if (offset == 0x14) {
            if (value & ISR_OVR_BIT) clear_overrun();
            isr_ &= ~(value & 0x7FFFFFFFu);
        } else if (offset == 0x18) {
            debounce_ticks_ = value;
        }
    }

    void inject_external_input(uint32_t pin_values) {
        if (check_and_flag_overrun()) return;
        pending_ext_idr_ = pin_values;
        schedule_busy(debounce_ticks_);
    }

    void reset() override {
        reset_clock_state();
        moder_ = 0;
        odr_ = 0;
        ext_idr_ = 0;
        pending_ext_idr_ = 0;
        ier_ = 0;
        isr_ = 0;
        debounce_ticks_ = 0;
    }

    [[nodiscard]] bool has_pending_irq() const noexcept override {
        return (isr_ & ier_) != 0;
    }

protected:
    void on_transfer_complete() override {
        update_pins([&]() { ext_idr_ = pending_ext_idr_; });
    }

private:
    template <typename Fn>
    void update_pins(Fn&& fn) {
        uint32_t old_pins = read(0x04, AccessSize::Word);
        fn();
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
};

} // namespace tinyarmsim::mmio
