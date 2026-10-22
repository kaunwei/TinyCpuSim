#pragma once

#include <cstdint>
#include <string>
#include "tinyarmsim/mmio/device.hpp"

namespace tinyarmsim::mmio {

// Virtual SPI Controller with attached W25QXX Flash slave
class SpiDevice : public MMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x40003000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;

    explicit SpiDevice(uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN)
        : base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "SPI0"; }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) return cr_;
        if (offset == 0x04) return sr_; // Bit 0: BSY, Bit 1: TXE, Bit 2: RXNE
        if (offset == 0x08) {
            uint32_t val = rx_data_;
            sr_ &= ~0x04u; // Clear RXNE
            return val;
        }
        if (offset == 0x0C) return cs_;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) {
            cr_ = value;
        } else if (offset == 0x08) {
            transfer_byte(static_cast<uint8_t>(value & 0xFF));
        } else if (offset == 0x0C) {
            cs_ = value;
            if (cs_ != 0) {
                // CS deasserted -> reset SPI flash command state
                cmd_state_ = 0;
            }
        }
    }

    void reset() override {
        cr_ = 0x03; // SPE | MSTR
        sr_ = 0x02; // TXE = 1
        rx_data_ = 0;
        cs_ = 1;    // CS high (inactive)
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
        sr_ |= 0x04; // RXNE = 1
        sr_ |= 0x02; // TXE = 1
    }

    uint32_t base_;
    uint32_t span_;
    uint32_t cr_{0x03};
    uint32_t sr_{0x02};
    uint8_t rx_data_{0};
    uint32_t cs_{1};
    int cmd_state_{0};
};

} // namespace tinyarmsim::mmio
