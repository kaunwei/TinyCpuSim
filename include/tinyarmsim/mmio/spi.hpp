#pragma once

#include <cstdint>
#include <string>
#include "tinyarmsim/mmio/device.hpp"

namespace tinyarmsim::mmio {

class SpiDevice : public ClockedMMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x40003000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;
    static constexpr uint64_t SCLK_BITS_PER_BYTE = 8ULL;

    explicit SpiDevice(uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN)
        : base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "SPI0"; }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) return cr_;
        if (offset == 0x04) {
            uint32_t status = sr_;
            if (is_busy()) {
                status |= 0x01u;
                status &= ~0x02u;
            } else {
                status &= ~0x01u;
            }
            if (has_overrun()) status |= 0x08u;
            return status;
        }
        if (offset == 0x08) {
            uint32_t val = rx_data_;
            sr_ &= ~0x04u;
            return val;
        }
        if (offset == 0x0C) return cs_;
        if (offset == 0x10) return clkdiv_;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) {
            cr_ = value;
        } else if (offset == 0x04) {
            if (value & 0x08u) clear_overrun();
        } else if (offset == 0x08) {
            if (check_and_flag_overrun()) return;
            pending_tx_byte_ = static_cast<uint8_t>(value & 0xFFu);
            sr_ &= ~(0x02u | 0x04u);
            schedule_busy(SCLK_BITS_PER_BYTE * static_cast<uint64_t>(clkdiv_));
        } else if (offset == 0x0C) {
            cs_ = value;
            if (cs_ != 0) cmd_state_ = 0;
        } else if (offset == 0x10) {
            clkdiv_ = value;
        }
    }

    void reset() override {
        reset_clock_state();
        cr_ = 0x03;
        sr_ = 0x02;
        rx_data_ = 0;
        cs_ = 1;
        clkdiv_ = 0;
        pending_tx_byte_ = 0;
        cmd_state_ = 0;
    }

protected:
    void on_transfer_complete() override {
        uint8_t byte = pending_tx_byte_;
        if (cs_ == 0) {
            if (cmd_state_ == 0) {
                if (byte == 0x9F) {
                    cmd_state_ = 1;
                    rx_data_ = 0xEF;
                } else {
                    rx_data_ = byte;
                }
            } else if (cmd_state_ == 1) {
                rx_data_ = 0x40;
                cmd_state_ = 2;
            } else if (cmd_state_ == 2) {
                rx_data_ = 0x18;
                cmd_state_ = 0;
            } else {
                rx_data_ = 0xFF;
            }
        } else {
            rx_data_ = 0xFF;
        }
        sr_ |= (0x04u | 0x02u);
    }

private:
    uint32_t base_;
    uint32_t span_;
    uint32_t cr_{0x03};
    uint32_t sr_{0x02};
    uint8_t rx_data_{0};
    uint32_t cs_{1};
    uint32_t clkdiv_{0};
    uint8_t pending_tx_byte_{0};
    int cmd_state_{0};
};

} // namespace tinyarmsim::mmio
