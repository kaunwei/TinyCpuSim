#pragma once

#include <string>
#include "tinyarmsim/dev/device.hpp"

namespace tinyarmsim::platform {

class ProxyMMIODevice : public mmio::MMIODevice {
public:
    ProxyMMIODevice(mmio::MMIODevice& target, uint32_t base, uint32_t span)
        : target_(target), base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return target_.device_name(); }

    [[nodiscard]] bool has_pending_irq() const noexcept override {
        return target_.has_pending_irq();
    }

    [[nodiscard]] uint32_t irq_line() const noexcept override {
        return target_.irq_line();
    }

    [[nodiscard]] uint32_t read(uint32_t offset, mmio::AccessSize size) override {
        return target_.read(offset, size);
    }

    void write(uint32_t offset, uint32_t value, mmio::AccessSize size) override {
        target_.write(offset, value, size);
    }

    void tick(uint64_t cycles) override {
        target_.tick(cycles);
    }

    void reset() override {
        target_.reset();
    }

private:
    mmio::MMIODevice& target_;
    uint32_t base_;
    uint32_t span_;
};

} // namespace tinyarmsim::platform
