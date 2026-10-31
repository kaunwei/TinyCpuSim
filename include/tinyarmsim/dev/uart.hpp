#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include "tinyarmsim/mmio/device.hpp"

namespace tinyarmsim::mmio {

class UartDevice : public ClockedMMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x40000000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;
    static constexpr uint64_t FRAME_BITS_8N1 = 10ULL; // 1 start + 8 data + 1 stop
    static constexpr size_t kDefaultRxCapacity = 64;

    explicit UartDevice(uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN, size_t rx_capacity = kDefaultRxCapacity)
        : base_(base), span_(span), rx_capacity_(rx_capacity) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "UART0"; }
    [[nodiscard]] uint32_t irq_line() const noexcept override { return 0; }

    [[nodiscard]] size_t rx_capacity() const noexcept { return rx_capacity_; }
    [[nodiscard]] size_t rx_size() const noexcept { return rx_fifo_.size(); }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) {
            if (rx_fifo_.empty()) return 0;
            uint8_t val = rx_fifo_.front();
            rx_fifo_.pop_front();
            return val;
        }
        if (offset == 0x04) {
            // bit 0: TX_READY, bit 1: RX_VALID, bit 2: TX_EMPTY, bit 3: BUSY, bit 4: OVR
            uint32_t status = is_busy() ? 0x08u : (0x01u | 0x04u);
            if (!rx_fifo_.empty()) status |= 0x02u;
            if (has_overrun()) status |= 0x10u;
            return status;
        }
        if (offset == 0x08) return control_;
        if (offset == 0x0C) return baud_;
        if (offset == 0x10) return clkdiv_;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) {
            if ((control_ & 0x01u) == 0) return;
            if (check_and_flag_overrun()) return;
            pending_tx_char_ = static_cast<char>(value & 0xFFu);
            schedule_busy(FRAME_BITS_8N1 * static_cast<uint64_t>(clkdiv_));
        } else if (offset == 0x04) {
            if (value & 0x10u) clear_overrun();
        } else if (offset == 0x08) {
            control_ = value;
        } else if (offset == 0x0C) {
            baud_ = value;
        } else if (offset == 0x10) {
            clkdiv_ = value;
        }
    }

    [[nodiscard]] uint64_t reset_generation() const noexcept { return reset_generation_; }

    void reset() override {
        ++reset_generation_;
        reset_clock_state();
        rx_fifo_.clear();
        tx_output_.clear();
        control_ = 0x03;
        baud_ = 115200;
        clkdiv_ = 0;
        pending_tx_char_ = 0;
    }

    [[nodiscard]] bool try_inject_rx(uint8_t byte) noexcept {
        if (rx_fifo_.size() >= rx_capacity_) {
            return false;
        }
        rx_fifo_.push_back(byte);
        return true;
    }

    void inject_rx(uint8_t byte) { (void)try_inject_rx(byte); }
    void inject_rx(const std::string& str) {
        for (char c : str) {
            if (!try_inject_rx(static_cast<uint8_t>(c))) {
                break;
            }
        }
    }

    [[nodiscard]] const std::string& get_tx_output() const noexcept { return tx_output_; }

    void set_tx_consumer(std::function<void(uint8_t)> consumer) {
        tx_consumer_ = std::move(consumer);
    }

    [[nodiscard]] bool has_pending_irq() const noexcept override {
        return ((control_ & 0x04u) != 0) && !rx_fifo_.empty();
    }

protected:
    void on_transfer_complete() override {
        const auto byte = static_cast<uint8_t>(pending_tx_char_);
        if (tx_consumer_) {
            tx_consumer_(byte);
        } else {
            tx_output_.push_back(pending_tx_char_);
        }
    }

private:
    uint64_t reset_generation_{0};
    uint32_t base_;
    uint32_t span_;
    uint32_t control_{0x03};
    uint32_t baud_{115200};
    uint32_t clkdiv_{0};
    char pending_tx_char_{0};
    size_t rx_capacity_{kDefaultRxCapacity};
    std::deque<uint8_t> rx_fifo_;
    std::string tx_output_;
    std::function<void(uint8_t)> tx_consumer_;
};

} // namespace tinyarmsim::mmio
