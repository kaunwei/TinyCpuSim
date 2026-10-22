#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "tinyarmsim/mmio/device.hpp"

namespace tinyarmsim::mmio {

// Virtual SPI Controller with attached W25QXX Flash slave
class SpiDevice : public MMIODevice {
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
            // Bit 0: BSY, Bit 1: TXE, Bit 2: RXNE, Bit 3: OVR
            uint32_t status = sr_;
            if (busy_ticks_ > 0) {
                status |= 0x01u;  // BSY = 1
                status &= ~0x02u; // TXE = 0
            } else {
                status &= ~0x01u; // BSY = 0
            }
            if (ovr_) status |= 0x08u;
            return status;
        }
        if (offset == 0x08) {
            uint32_t val = rx_data_;
            sr_ &= ~0x04u; // Clear RXNE
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
            if (value & 0x08u) ovr_ = false; // Write-1-to-clear OVR
        } else if (offset == 0x08) {
            if (busy_ticks_ > 0) {
                ovr_ = true;
                return;
            }
            uint8_t byte = static_cast<uint8_t>(value & 0xFFu);
            if (clkdiv_ == 0) {
                transfer_byte(byte);
            } else {
                pending_tx_byte_ = byte;
                sr_ &= ~(0x02u | 0x04u); // Clear TXE & RXNE while shifting
                busy_ticks_ = SCLK_BITS_PER_BYTE * static_cast<uint64_t>(clkdiv_);
            }
        } else if (offset == 0x0C) {
            cs_ = value;
            if (cs_ != 0) {
                cmd_state_ = 0;
            }
        } else if (offset == 0x10) {
            clkdiv_ = value;
        }
    }

    void tick(uint64_t cycles) override {
        if (busy_ticks_ > 0) {
            if (cycles >= busy_ticks_) {
                busy_ticks_ = 0;
                transfer_byte(pending_tx_byte_);
            } else {
                busy_ticks_ -= cycles;
            }
        }
    }

    void reset() override {
        cr_ = 0x03; // SPE | MSTR
        sr_ = 0x02; // TXE = 1
        rx_data_ = 0;
        cs_ = 1;    // CS high (inactive)
        clkdiv_ = 0;
        busy_ticks_ = 0;
        pending_tx_byte_ = 0;
        ovr_ = false;
        cmd_state_ = 0;
    }

private:
    void transfer_byte(uint8_t byte) {
        if (cs_ == 0) { // Active transfer
            if (cmd_state_ == 0) {
                if (byte == 0x9F) { // Read JEDEC ID
                    cmd_state_ = 1;
                    rx_data_ = 0xEF; // Winbond Manufacturer ID
                } else {
                    rx_data_ = byte; // Loopback
                }
            } else if (cmd_state_ == 1) {
                rx_data_ = 0x40; // Memory type
                cmd_state_ = 2;
            } else if (cmd_state_ == 2) {
                rx_data_ = 0x18; // Capacity (128M-bit)
                cmd_state_ = 0;
            } else {
                rx_data_ = 0xFF;
            }
        } else {
            rx_data_ = 0xFF;
        }
        sr_ |= 0x04u; // RXNE = 1
        sr_ |= 0x02u; // TXE = 1
    }

    uint32_t base_;
    uint32_t span_;
    uint32_t cr_{0x03};
    uint32_t sr_{0x02};
    uint8_t rx_data_{0};
    uint32_t cs_{1};
    uint32_t clkdiv_{0};
    uint64_t busy_ticks_{0};
    uint8_t pending_tx_byte_{0};
    bool ovr_{false};
    int cmd_state_{0};
};

} // namespace tinyarmsim::mmio
