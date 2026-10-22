#pragma once

#include <cstdint>
#include <string>
#include "tinyarmsim/mmio/device.hpp"

namespace tinyarmsim::mmio {

// User-Defined Hardware Accelerator Example
class CustomDevice : public MMIODevice {
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
            // bit 0: READY/DONE, bit 1: BUSY, bit 2: OVR
            uint32_t st = status_;
            if (busy_ticks_ > 0) {
                st |= 0x02u;  // BUSY
                st &= ~0x01u; // Not READY
            }
            if (ovr_) st |= 0x04u;
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
            if (busy_ticks_ > 0) {
                ovr_ = true;
                return;
            }
            cmd_ = value;
            if (latency_ == 0) {
                execute_cmd();
            } else {
                status_ &= ~0x01u;
                busy_ticks_ = latency_;
            }
        } else if (offset == 0x10) {
            if (value & 0x04u) ovr_ = false; // Write-1-to-clear OVR
        } else if (offset == 0x14) {
            latency_ = value;
        }
    }

    void tick(uint64_t cycles) override {
        if (busy_ticks_ > 0) {
            if (cycles >= busy_ticks_) {
                busy_ticks_ = 0;
                execute_cmd();
            } else {
                busy_ticks_ -= cycles;
            }
        }
    }

    void reset() override {
        reg_a_ = 0;
        reg_b_ = 0;
        cmd_ = 0;
        result_ = 0;
        status_ = 0;
        latency_ = 0;
        busy_ticks_ = 0;
        ovr_ = false;
    }

private:
    void execute_cmd() {
        if (cmd_ == 1) { // ADD
            result_ = reg_a_ + reg_b_;
        } else if (cmd_ == 2) { // MUL
            result_ = reg_a_ * reg_b_;
        } else if (cmd_ == 3) { // XOR
            result_ = reg_a_ ^ reg_b_;
        }
        status_ = 0x01u; // DONE / READY
    }

    uint32_t base_;
    uint32_t span_;
    uint32_t reg_a_{0};
    uint32_t reg_b_{0};
    uint32_t cmd_{0};
    uint32_t result_{0};
    uint32_t status_{0};
    uint32_t latency_{0};
    uint64_t busy_ticks_{0};
    bool ovr_{false};
};

} // namespace tinyarmsim::mmio
