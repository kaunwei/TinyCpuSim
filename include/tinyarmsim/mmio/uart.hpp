#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include "tinyarmsim/mmio/device.hpp"

namespace tinyarmsim::mmio {

class UartDevice : public MMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x40000000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;
    static constexpr uint64_t FRAME_BITS_8N1 = 10ULL; // 1 start + 8 data + 1 stop

    explicit UartDevice(uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN)
        : base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "UART0"; }
    [[nodiscard]] uint32_t irq_line() const noexcept override { return 0; }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        // Offset 0x00: DATA
        if (offset == 0x00) {
            if (rx_fifo_.empty()) return 0;
            uint8_t val = rx_fifo_.front();
            rx_fifo_.pop_front();
            return val;
        }
        // Offset 0x04: STATUS
        // bit 0: TX_READY, bit 1: RX_VALID, bit 2: TX_EMPTY, bit 3: BUSY, bit 4: OVR
        if (offset == 0x04) {
            uint32_t status = 0;
            if (tx_busy_ticks_ == 0) {
                status |= (0x01u | 0x04u); // TX_READY | TX_EMPTY
            } else {
                status |= 0x08u; // BUSY
            }
            if (!rx_fifo_.empty()) status |= 0x02u; // RX_VALID
            if (ovr_) status |= 0x10u; // OVR
            return status;
        }
        // Offset 0x08: CONTROL
        if (offset == 0x08) return control_;
        // Offset 0x0C: BAUD
        if (offset == 0x0C) return baud_;
        // Offset 0x10: CLKDIV (Bit clock divider; 0 = 0-cycle fast mode, >=1 = 10 * clkdiv ticks per frame)
        if (offset == 0x10) return clkdiv_;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        // Offset 0x00: DATA
        if (offset == 0x00) {
            if ((control_ & 0x01u) == 0) return; // TX disabled
            if (tx_busy_ticks_ > 0) {
                ovr_ = true; // Overrun: wrote DATA while UART shift register is busy
                return;
            }
            char ch = static_cast<char>(value & 0xFFu);
            if (clkdiv_ == 0) {
                tx_output_.push_back(ch);
            } else {
                pending_tx_char_ = ch;
                tx_busy_ticks_ = FRAME_BITS_8N1 * static_cast<uint64_t>(clkdiv_);
            }
        } else if (offset == 0x04) {
            // Write-1-to-clear OVR (bit 4)
            if (value & 0x10u) ovr_ = false;
        } else if (offset == 0x08) {
            control_ = value;
        } else if (offset == 0x0C) {
            baud_ = value;
        } else if (offset == 0x10) {
            clkdiv_ = value;
        }
    }

    void tick(uint64_t cycles) override {
        if (tx_busy_ticks_ > 0) {
            if (cycles >= tx_busy_ticks_) {
                tx_busy_ticks_ = 0;
                tx_output_.push_back(pending_tx_char_);
            } else {
                tx_busy_ticks_ -= cycles;
            }
        }
    }

    void reset() override {
        rx_fifo_.clear();
        tx_output_.clear();
        control_ = 0x03; // Default TX_EN and RX_EN enabled
        baud_ = 115200;
        clkdiv_ = 0;
        tx_busy_ticks_ = 0;
        pending_tx_char_ = 0;
        ovr_ = false;
    }

    void inject_rx(uint8_t byte) {
        rx_fifo_.push_back(byte);
    }

    void inject_rx(const std::string& str) {
        for (char c : str) rx_fifo_.push_back(static_cast<uint8_t>(c));
    }

    [[nodiscard]] const std::string& get_tx_output() const noexcept {
        return tx_output_;
    }

    [[nodiscard]] bool has_pending_irq() const noexcept override {
        bool rx_irq_en = (control_ & 0x04u) != 0;
        return rx_irq_en && !rx_fifo_.empty();
    }

private:
    uint32_t base_;
    uint32_t span_;
    uint32_t control_{0x03};
    uint32_t baud_{115200};
    uint32_t clkdiv_{0};
    uint64_t tx_busy_ticks_{0};
    char pending_tx_char_{0};
    bool ovr_{false};
    std::deque<uint8_t> rx_fifo_;
    std::string tx_output_;
};

} // namespace tinyarmsim::mmio
