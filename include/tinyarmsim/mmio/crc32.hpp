#pragma once

#include <cstdint>
#include <string>
#include "tinyarmsim/mmio/device.hpp"

namespace tinyarmsim::mmio {

// Hardware CRC-32 Accelerator (IEEE 802.3 Ethernet Polynomial 0x04C11DB7)
class Crc32Device : public MMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x40004000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;
    static constexpr uint32_t CRC32_POLY = 0x04C11DB7;

    explicit Crc32Device(uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN)
        : base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "CRC32"; }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) return crc_;
        if (offset == 0x04) return init_val_;
        if (offset == 0x08) return cr_;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, AccessSize size) override {
        if (offset == 0x00) {
            if (size == AccessSize::Byte) {
                update_byte(static_cast<uint8_t>(value & 0xFF));
            } else if (size == AccessSize::HalfWord) {
                update_byte(static_cast<uint8_t>(value & 0xFF));
                update_byte(static_cast<uint8_t>((value >> 8) & 0xFF));
            } else {
                update_byte(static_cast<uint8_t>(value & 0xFF));
                update_byte(static_cast<uint8_t>((value >> 8) & 0xFF));
                update_byte(static_cast<uint8_t>((value >> 16) & 0xFF));
                update_byte(static_cast<uint8_t>((value >> 24) & 0xFF));
            }
        } else if (offset == 0x04) {
            init_val_ = value;
            crc_ = init_val_;
        } else if (offset == 0x08) {
            cr_ = value;
            if (cr_ & 0x01) { // Reset
                crc_ = init_val_;
            }
        }
    }

    void reset() override {
        init_val_ = 0xFFFFFFFF;
        crc_ = 0xFFFFFFFF;
        cr_ = 0;
    }

private:
    void update_byte(uint8_t byte) {
        crc_ ^= (static_cast<uint32_t>(byte) << 24);
        for (int i = 0; i < 8; ++i) {
            if (crc_ & 0x80000000u) {
                crc_ = (crc_ << 1) ^ CRC32_POLY;
            } else {
                crc_ <<= 1;
            }
        }
    }

    uint32_t base_;
    uint32_t span_;
    uint32_t init_val_{0xFFFFFFFF};
    uint32_t crc_{0xFFFFFFFF};
    uint32_t cr_{0};
};

} // namespace tinyarmsim::mmio
