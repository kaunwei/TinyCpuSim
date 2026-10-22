#pragma once

#include <array>
#include <cstdint>
#include <string>
#include "tinyarmsim/mmio/device.hpp"

namespace tinyarmsim::mmio {

// Virtual I2C Controller with attached AT24C02 256-byte EEPROM at 7-bit addr 0x50
class I2CDevice : public MMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x40002000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;
    static constexpr uint8_t EEPROM_ADDR = 0x50;

    explicit I2CDevice(uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN)
        : base_(base), span_(span) {
        eeprom_data_.fill(0xFF);
    }

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "I2C0"; }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) return cr_;
        if (offset == 0x04) return sr_; // bit 0: busy, bit 1: ack, bit 2: rxne, bit 3: txe
        if (offset == 0x08) {
            // Read DR
            uint8_t byte = rx_data_;
            sr_ &= ~0x04u; // Clear RXNE
            return byte;
        }
        if (offset == 0x0C) return slave_addr_;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) {
            cr_ = value;
            execute_control(value);
        } else if (offset == 0x08) {
            // Write DR
            write_data_byte(static_cast<uint8_t>(value & 0xFF));
        } else if (offset == 0x0C) {
            slave_addr_ = static_cast<uint8_t>(value & 0x7F);
        }
    }

    void reset() override {
        cr_ = 0;
        sr_ = 0x08; // TXE = 1
        rx_data_ = 0;
        slave_addr_ = 0;
        state_ = State::Idle;
        eeprom_ptr_ = 0;
    }

    [[nodiscard]] uint8_t read_eeprom_direct(uint8_t addr) const noexcept {
        return eeprom_data_[addr];
    }

private:
    enum class State { Idle, AddrPhase, MemAddrPhase, DataWrite, DataRead };

    void execute_control(uint32_t cr) {
        // Bit 0: START, Bit 1: STOP, Bit 2: READ_TRIGGER
        if (cr & 0x01) { // START
            if (slave_addr_ == EEPROM_ADDR) {
                state_ = State::MemAddrPhase;
            } else {
                state_ = State::AddrPhase;
            }
            sr_ |= 0x02; // ACK
        }
        if (cr & 0x02) { // STOP
            state_ = State::Idle;
        }
        if (cr & 0x04) { // Read single byte from current EEPROM pointer
            if (slave_addr_ == EEPROM_ADDR) {
                rx_data_ = eeprom_data_[eeprom_ptr_++];
                sr_ |= 0x04; // RXNE = 1
            }
        }
    }

    void write_data_byte(uint8_t byte) {
        if (state_ == State::AddrPhase) {
            // If slave addr matches EEPROM
            if (slave_addr_ == EEPROM_ADDR || (byte >> 1) == EEPROM_ADDR || byte == EEPROM_ADDR) {
                slave_addr_ = EEPROM_ADDR;
                state_ = State::MemAddrPhase;
            }
        } else if (state_ == State::MemAddrPhase) {
            eeprom_ptr_ = byte;
            state_ = State::DataWrite;
        } else if (state_ == State::DataWrite) {
            if (slave_addr_ == EEPROM_ADDR) {
                eeprom_data_[eeprom_ptr_++] = byte;
            }
        }
        sr_ |= 0x08; // TXE = 1
        sr_ |= 0x02; // ACK = 1
    }

    uint32_t base_;
    uint32_t span_;
    uint32_t cr_{0};
    uint32_t sr_{0x08}; // TXE = 1
    uint8_t rx_data_{0};
    uint8_t slave_addr_{0};
    uint8_t eeprom_ptr_{0};
    State state_{State::Idle};
    std::array<uint8_t, 256> eeprom_data_{};
};

} // namespace tinyarmsim::mmio
