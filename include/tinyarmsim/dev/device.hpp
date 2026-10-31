#pragma once

#include <cstdint>
#include <string>

namespace tinyarmsim::mmio {

enum class AccessSize : uint8_t {
    Byte = 1,
    HalfWord = 2,
    Word = 4
};

class MMIODevice {
public:
    virtual ~MMIODevice() = default;

    [[nodiscard]] virtual uint32_t base_address() const noexcept = 0;
    [[nodiscard]] virtual uint32_t address_span() const noexcept = 0;

    [[nodiscard]] virtual uint32_t read(uint32_t offset, AccessSize size) = 0;
    virtual void write(uint32_t offset, uint32_t value, AccessSize size) = 0;

    virtual void tick([[maybe_unused]] uint64_t cycles) {}
    virtual void reset() {}

    [[nodiscard]] virtual bool has_pending_irq() const noexcept { return false; }
    [[nodiscard]] virtual uint32_t irq_line() const noexcept { return 0; }
    [[nodiscard]] virtual std::string device_name() const { return "MMIODevice"; }
};

// Deep base module encapsulating protocol bit-clock countdowns, BUSY state, and Overrun (OVR) collision detection
class ClockedMMIODevice : public MMIODevice {
public:
    void tick(uint64_t cycles) override {
        if (busy_ticks_ > 0) {
            if (cycles >= busy_ticks_) {
                busy_ticks_ = 0;
                on_transfer_complete();
            } else {
                busy_ticks_ -= cycles;
            }
        }
    }

    [[nodiscard]] bool is_busy() const noexcept { return busy_ticks_ > 0; }
    [[nodiscard]] bool has_overrun() const noexcept { return ovr_; }

protected:
    virtual void on_transfer_complete() = 0;

    // Returns true and latches OVR if device is currently busy
    bool check_and_flag_overrun() noexcept {
        if (busy_ticks_ > 0) {
            ovr_ = true;
            return true;
        }
        return false;
    }

    void schedule_busy(uint64_t ticks) {
        if (ticks == 0) {
            on_transfer_complete();
        } else {
            busy_ticks_ = ticks;
        }
    }

    void clear_overrun() noexcept { ovr_ = false; }

    void reset_clock_state() noexcept {
        busy_ticks_ = 0;
        ovr_ = false;
    }

private:
    uint64_t busy_ticks_{0};
    bool ovr_{false};
};

} // namespace tinyarmsim::mmio
