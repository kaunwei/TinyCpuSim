#pragma once

#include <array>
#include <cstdint>
#include <string>
#include "tinyarmsim/dev/device.hpp"

namespace tinyarmsim::mmio {

class I2CDevice : public ClockedMMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x40002000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;
    static constexpr uint8_t EEPROM_ADDR = 0x50;
    static constexpr uint64_t SCL_BITS_PER_BYTE = 9ULL;
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
            uint32_t status = sr_;
            if (is_busy()) {
                status |= 0x01u;
                status &= ~0x08u;
            } else {
                status &= ~0x01u;
            }
            if (has_overrun()) status |= 0x10u;
            return status;
        }
        if (offset == 0x08) {
            uint8_t byte = rx_data_;
            sr_ &= ~0x04u;
            return byte;
        }
        if (offset == 0x0C) return slave_addr_;
        if (offset == 0x10) return clkdiv_;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) {
            if (check_and_flag_overrun()) return;
            cr_ = value;
            pending_op_ = PendingOp::Control;
            pending_val_ = value;
            uint64_t bits = (value & 0x04u) ? SCL_BITS_PER_BYTE : SCL_BITS_START_STOP;
            schedule_busy(bits * static_cast<uint64_t>(clkdiv_));
        } else if (offset == 0x04) {
            if (value & 0x10u) clear_overrun();
        } else if (offset == 0x08) {
            if (check_and_flag_overrun()) return;
            pending_op_ = PendingOp::WriteByte;
            pending_val_ = value & 0xFFu;
            sr_ &= ~(0x02u | 0x08u);
            schedule_busy(SCL_BITS_PER_BYTE * static_cast<uint64_t>(clkdiv_));
        } else if (offset == 0x0C) {
            slave_addr_ = static_cast<uint8_t>(value & 0x7Fu);
        } else if (offset == 0x10) {
            clkdiv_ = value;
        }
    }

    void reset() override {
        reset_clock_state();
        cr_ = 0;
        sr_ = 0x08;
        rx_data_ = 0;
        slave_addr_ = 0;
        clkdiv_ = 0;
        pending_op_ = PendingOp::None;
        pending_val_ = 0;
        state_ = State::Idle;
        eeprom_ptr_ = 0;
    }

    [[nodiscard]] uint8_t read_eeprom_direct(uint8_t addr) const noexcept {
        return eeprom_data_[addr];
    }

protected:
    void on_transfer_complete() override {
        if (pending_op_ == PendingOp::Control) {
            execute_control(pending_val_);
        } else if (pending_op_ == PendingOp::WriteByte) {
            write_data_byte(static_cast<uint8_t>(pending_val_ & 0xFFu));
        }
        pending_op_ = PendingOp::None;
    }

private:
    enum class State { Idle, AddrPhase, MemAddrPhase, DataWrite, DataRead };
    enum class PendingOp { None, Control, WriteByte };

    void execute_control(uint32_t cr) {
        if (cr & 0x01u) {
            if (slave_addr_ == EEPROM_ADDR) {
                state_ = State::MemAddrPhase;
            } else {
                state_ = State::AddrPhase;
            }
            sr_ |= 0x02u;
        }
        if (cr & 0x02u) {
            state_ = State::Idle;
        }
        if (cr & 0x04u) {
            if (slave_addr_ == EEPROM_ADDR) {
                rx_data_ = eeprom_data_[eeprom_ptr_++];
                sr_ |= (0x04u | 0x02u);
            }
        }
        sr_ |= 0x08u;
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
        sr_ |= (0x08u | 0x02u);
    }

    uint32_t base_;
    uint32_t span_;
    uint32_t cr_{0};
    uint32_t sr_{0x08};
    uint8_t rx_data_{0};
    uint8_t slave_addr_{0};
    uint32_t clkdiv_{0};
    PendingOp pending_op_{PendingOp::None};
    uint32_t pending_val_{0};
    uint8_t eeprom_ptr_{0};
    State state_{State::Idle};
    std::array<uint8_t, 256> eeprom_data_{};
};

} // namespace tinyarmsim::mmio
