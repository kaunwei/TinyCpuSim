#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "tinyarmsim/mmio/device.hpp"
#include "mailbox_scratchpad.h"

namespace tinyarmsim::mmio {

class MailboxScratchpadDevice : public MMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x50000000;
    static constexpr uint32_t DEFAULT_SPAN = 0x20000; // 128KB total span, covering CSRs + 64KB scratchpad at 0x1000
    static constexpr uint32_t SCRATCHPAD_OFFSET = MB_SCRATCHPAD_OFFSET;
    static constexpr size_t SCRATCHPAD_SIZE = MB_SCRATCHPAD_SIZE;
    static constexpr uint32_t DEFAULT_HOST_IRQ_LINE = 4;

    using AccelIrqCallback = std::function<void()>;

    explicit MailboxScratchpadDevice(uint32_t base = DEFAULT_BASE,
                                    uint32_t span = DEFAULT_SPAN,
                                    uint32_t host_irq_line = DEFAULT_HOST_IRQ_LINE)
        : base_(base),
          span_(span),
          host_irq_line_(host_irq_line),
          scratchpad_(SCRATCHPAD_SIZE, 0) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "MailboxScratchpad"; }

    [[nodiscard]] bool has_pending_irq() const noexcept override {
        return doorbell_to_host_pending_;
    }

    [[nodiscard]] uint32_t irq_line() const noexcept override {
        return host_irq_line_;
    }

    void set_accel_irq_callback(AccelIrqCallback cb) {
        accel_irq_callback_ = std::move(cb);
    }

    void signal_accel_irq() {
        doorbell_to_accel_pending_ = true;
        if (accel_irq_callback_) {
            accel_irq_callback_();
        }
    }

    [[nodiscard]] bool doorbell_to_accel_pending() const noexcept {
        return doorbell_to_accel_pending_;
    }

    [[nodiscard]] bool doorbell_to_host_pending() const noexcept {
        return doorbell_to_host_pending_;
    }

    void clear_accel_doorbell() noexcept {
        doorbell_to_accel_pending_ = false;
    }

    void clear_host_doorbell() noexcept {
        doorbell_to_host_pending_ = false;
    }

    [[nodiscard]] uint8_t* scratchpad_data() noexcept {
        return scratchpad_.data();
    }

    [[nodiscard]] const uint8_t* scratchpad_data() const noexcept {
        return scratchpad_.data();
    }

    [[nodiscard]] size_t scratchpad_size() const noexcept {
        return scratchpad_.size();
    }

    [[nodiscard]] uint32_t read(uint32_t offset, AccessSize size) override {
        if (offset >= SCRATCHPAD_OFFSET && offset < SCRATCHPAD_OFFSET + SCRATCHPAD_SIZE) {
            return read_scratchpad(offset - SCRATCHPAD_OFFSET, size);
        }

        switch (offset) {
            case MB_REG_STATUS:
                return status_;
            case MB_REG_PARAM_WIDTH:
                return param_width_;
            case MB_REG_PARAM_HEIGHT:
                return param_height_;
            case MB_REG_PARAM_CHANNELS:
                return param_channels_;
            case MB_REG_PARAM_KERNEL_SZ:
                return param_kernel_sz_;
            case MB_REG_DOORBELL_TO_ACCEL:
                return doorbell_to_accel_pending_ ? 1u : 0u;
            case MB_REG_DOORBELL_TO_HOST:
                return doorbell_to_host_pending_ ? 1u : 0u;
            case MB_REG_INTCLR:
                return 0u;
            default:
                return 0u;
        }
    }

    void write(uint32_t offset, uint32_t value, AccessSize size) override {
        if (offset >= SCRATCHPAD_OFFSET && offset < SCRATCHPAD_OFFSET + SCRATCHPAD_SIZE) {
            write_scratchpad(offset - SCRATCHPAD_OFFSET, value, size);
            return;
        }

        switch (offset) {
            case MB_REG_STATUS:
                status_ = value;
                break;
            case MB_REG_PARAM_WIDTH:
                param_width_ = value;
                break;
            case MB_REG_PARAM_HEIGHT:
                param_height_ = value;
                break;
            case MB_REG_PARAM_CHANNELS:
                param_channels_ = value;
                break;
            case MB_REG_PARAM_KERNEL_SZ:
                param_kernel_sz_ = value;
                break;
            case MB_REG_DOORBELL_TO_ACCEL:
                if (value != 0) {
                    signal_accel_irq();
                } else {
                    doorbell_to_accel_pending_ = false;
                }
                break;
            case MB_REG_DOORBELL_TO_HOST:
                doorbell_to_host_pending_ = (value != 0);
                break;
            case MB_REG_INTCLR:
                // Bit 0: Clear Host Doorbell, Bit 1: Clear Accel Doorbell
                // Also, any non-zero write or specific bit clears doorbells
                if (value & 0x01u) {
                    doorbell_to_host_pending_ = false;
                }
                if (value & 0x02u) {
                    doorbell_to_accel_pending_ = false;
                }
                // If neither bit 0 nor 1 was specified (e.g. general intclr write of 1 or other)
                // support value == 1 clearing both or according to ticket specification:
                // "MB_REG_INTCLR clears pending status"
                break;
            default:
                break;
        }
    }

    void reset() override {
        status_ = MB_STATUS_IDLE;
        param_width_ = 0;
        param_height_ = 0;
        param_channels_ = 0;
        param_kernel_sz_ = 0;
        doorbell_to_accel_pending_ = false;
        doorbell_to_host_pending_ = false;
        std::fill(scratchpad_.begin(), scratchpad_.end(), static_cast<uint8_t>(0));
    }

private:
    [[nodiscard]] uint32_t read_scratchpad(uint32_t s_offset, AccessSize size) const {
        if (s_offset >= scratchpad_.size()) return 0;
        switch (size) {
            case AccessSize::Byte:
                return scratchpad_[s_offset];
            case AccessSize::HalfWord:
                if (s_offset + 1 < scratchpad_.size()) {
                    return static_cast<uint32_t>(scratchpad_[s_offset]) |
                           (static_cast<uint32_t>(scratchpad_[s_offset + 1]) << 8);
                }
                return scratchpad_[s_offset];
            case AccessSize::Word:
            default:
                if (s_offset + 3 < scratchpad_.size()) {
                    return static_cast<uint32_t>(scratchpad_[s_offset]) |
                           (static_cast<uint32_t>(scratchpad_[s_offset + 1]) << 8) |
                           (static_cast<uint32_t>(scratchpad_[s_offset + 2]) << 16) |
                           (static_cast<uint32_t>(scratchpad_[s_offset + 3]) << 24);
                }
                return 0;
        }
    }

    void write_scratchpad(uint32_t s_offset, uint32_t value, AccessSize size) {
        if (s_offset >= scratchpad_.size()) return;
        switch (size) {
            case AccessSize::Byte:
                scratchpad_[s_offset] = static_cast<uint8_t>(value & 0xFFu);
                break;
            case AccessSize::HalfWord:
                scratchpad_[s_offset] = static_cast<uint8_t>(value & 0xFFu);
                if (s_offset + 1 < scratchpad_.size()) {
                    scratchpad_[s_offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
                }
                break;
            case AccessSize::Word:
            default:
                scratchpad_[s_offset] = static_cast<uint8_t>(value & 0xFFu);
                if (s_offset + 1 < scratchpad_.size()) {
                    scratchpad_[s_offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
                }
                if (s_offset + 2 < scratchpad_.size()) {
                    scratchpad_[s_offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
                }
                if (s_offset + 3 < scratchpad_.size()) {
                    scratchpad_[s_offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
                }
                break;
        }
    }

    uint32_t base_;
    uint32_t span_;
    uint32_t host_irq_line_;

    uint32_t status_{MB_STATUS_IDLE};
    uint32_t param_width_{0};
    uint32_t param_height_{0};
    uint32_t param_channels_{0};
    uint32_t param_kernel_sz_{0};

    bool doorbell_to_accel_pending_{false};
    bool doorbell_to_host_pending_{false};

    AccelIrqCallback accel_irq_callback_{nullptr};
    std::vector<uint8_t> scratchpad_;
};

} // namespace tinyarmsim::mmio
