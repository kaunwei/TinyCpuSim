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
    static constexpr uint64_t SCL_BITS_PER_BYTE = 9ULL; // 8 data bits + 1 ACK bit
    static constexpr uint64_t SCL_BITS_START_STOP = 2ULL;

    explicit I2CDevice(uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN)
        : base_(base), span_(span) {
        eeprom_data_.fill(0xFF);
    }

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "I2C0"; }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) return cr_;
        if (offset == 0x04) {
            // bit 0: BUSY, bit 1: ACK, bit 2: RXNE, bit 3: TXE, bit 4: OVR
            uint32_t status = sr_;
            if (busy_ticks_ > 0) {
                status |= 0x01u;  // BUSY = 1
                status &= ~0x08u; // TXE = 0 while shifting
            } else {
                status &= ~0x01u; // BUSY = 0
            }
            if (ovr_) status |= 0x10u;
            return status;
        }
        if (offset == 0x08) {
            uint8_t byte = rx_data_;
            sr_ &= ~0x04u; // Clear RXNE
            return byte;
        }
        if (offset == 0x0C) return slave_addr_;
        if (offset == 0x10) return clkdiv_;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) {
            if (busy_ticks_ > 0) {
                ovr_ = true;
                return;
            }
            cr_ = value;
            if (clkdiv_ == 0) {
                execute_control(value);
            } else {
                pending_op_ = PendingOp::Control;
                pending_val_ = value;
                uint64_t bits = (value & 0x04u) ? SCL_BITS_PER_BYTE : SCL_BITS_START_STOP;
                busy_ticks_ = bits * static_cast<uint64_t>(clkdiv_);
            }
        } else if (offset == 0x04) {
            if (value & 0x10u) ovr_ = false; // Write-1-to-clear OVR
        } else if (offset == 0x08) {
            if (busy_ticks_ > 0) {
                ovr_ = true;
                return;
            }
            uint8_t byte = static_cast<uint8_t>(value & 0xFFu);
            if (clkdiv_ == 0) {
                write_data_byte(byte);
            } else {
                pending_op_ = PendingOp::WriteByte;
                pending_val_ = byte;
                sr_ &= ~(0x02u | 0x08u); // Clear ACK & TXE during transfer
                busy_ticks_ = SCL_BITS_PER_BYTE * static_cast<uint64_t>(clkdiv_);
            }
        } else if (offset == 0x0C) {
            slave_addr_ = static_cast<uint8_t>(value & 0x7Fu);
        } else if (offset == 0x10) {
            clkdiv_ = value;
        }
    }

    void tick(uint64_t cycles) override {
        if (busy_ticks_ > 0) {
            if (cycles >= busy_ticks_) {
                busy_ticks_ = 0;
                if (pending_op_ == PendingOp::Control) {
                    execute_control(pending_val_);
                } else if (pending_op_ == PendingOp::WriteByte) {
                    write_data_byte(static_cast<uint8_t>(pending_val_ & 0xFFu));
                }
                pending_op_ = PendingOp::None;
            } else {
                busy_ticks_ -= cycles;
            }
        }
    }

    void reset() override {
        cr_ = 0;
        sr_ = 0x08; // TXE = 1
        rx_data_ = 0;
        slave_addr_ = 0;
        clkdiv_ = 0;
        busy_ticks_ = 0;
        pending_op_ = PendingOp::None;
        pending_val_ = 0;
        ovr_ = false;
        state_ = State::Idle;
        eeprom_ptr_ = 0;
    }

    [[nodiscard]] uint8_t read_eeprom_direct(uint8_t addr) const noexcept {
        return eeprom_data_[addr];
    }

private:
    enum class State { Idle, AddrPhase, MemAddrPhase, DataWrite, DataRead };
    enum class PendingOp { None, Control, WriteByte };

    void execute_control(uint32_t cr) {
        if (cr & 0x01u) { // START
            if (slave_addr_ == EEPROM_ADDR) {
                state_ = State::MemAddrPhase;
            } else {
                state_ = State::AddrPhase;
            }
            sr_ |= 0x02u; // ACK
        }
        if (cr & 0x02u) { // STOP
            state_ = State::Idle;
        }
        if (cr & 0x04u) { // Read single byte from current EEPROM pointer
            if (slave_addr_ == EEPROM_ADDR) {
                rx_data_ = eeprom_data_[eeprom_ptr_++];
                sr_ |= 0x04u; // RXNE = 1
                sr_ |= 0x02u; // ACK = 1
            }
        }
        sr_ |= 0x08u; // TXE = 1
    }

    void write_data_byte(uint8_t byte) {
        if (state_ == State::AddrPhase) {
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
        sr_ |= 0x08u; // TXE = 1
        sr_ |= 0x02u; // ACK = 1
    }

    uint32_t base_;
    uint32_t span_;
    uint32_t cr_{0};
    uint32_t sr_{0x08}; // TXE = 1
    uint8_t rx_data_{0};
    uint8_t slave_addr_{0};
    uint32_t clkdiv_{0};
    uint64_t busy_ticks_{0};
    PendingOp pending_op_{PendingOp::None};
    uint32_t pending_val_{0};
    bool ovr_{false};
    uint8_t eeprom_ptr_{0};
    State state_{State::Idle};
    std::array<uint8_t, 256> eeprom_data_{};
};

} // namespace tinyarmsim::mmio
