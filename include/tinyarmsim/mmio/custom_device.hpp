#pragma once

#include <cstdint>
#include <string>
#include "tinyarmsim/mmio/device.hpp"

namespace tinyarmsim::mmio {

class CustomDevice : public ClockedMMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x50000000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;

    explicit CustomDevice(uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN)
        : base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "CustomAccel"; }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) return reg_a_;
        if (offset == 0x04) return reg_b_;
        if (offset == 0x08) return cmd_;
        if (offset == 0x0C) return result_;
        if (offset == 0x10) {
            uint32_t st = status_;
            if (is_busy()) {
                st |= 0x02u;
                st &= ~0x01u;
            }
            if (has_overrun()) st |= 0x04u;
            return st;
        }
        if (offset == 0x14) return latency_;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) {
            reg_a_ = value;
        } else if (offset == 0x04) {
            reg_b_ = value;
        } else if (offset == 0x08) {
            if (check_and_flag_overrun()) return;
            cmd_ = value;
            status_ &= ~0x01u;
            schedule_busy(latency_);
        } else if (offset == 0x10) {
            if (value & 0x04u) clear_overrun();
        } else if (offset == 0x14) {
            latency_ = value;
        }
    }

    void reset() override {
        reset_clock_state();
        reg_a_ = 0;
        reg_b_ = 0;
        cmd_ = 0;
        result_ = 0;
        status_ = 0;
        latency_ = 0;
    }

protected:
    void on_transfer_complete() override {
        if (cmd_ == 1) {
            result_ = reg_a_ + reg_b_;
        } else if (cmd_ == 2) {
            result_ = reg_a_ * reg_b_;
        } else if (cmd_ == 3) {
            result_ = reg_a_ ^ reg_b_;
        }
        status_ = 0x01u;
    }

private:
    uint32_t base_;
    uint32_t span_;
    uint32_t reg_a_{0};
    uint32_t reg_b_{0};
    uint32_t cmd_{0};
    uint32_t result_{0};
    uint32_t status_{0};
    uint32_t latency_{0};
};

} // namespace tinyarmsim::mmio
