#pragma once

#include <cstdint>
#include <string>
#include "tinyarmsim/cpu/faults.hpp"
#include "tinyarmsim/memory/memory_bus.hpp"
#include "tinyarmsim/dev/device.hpp"

namespace tinyarmsim::mmio {

// Bus-Mastering Direct Memory Access Controller (Mem2Mem, Mem2Periph, Periph2Mem)
class DmaDevice : public MMIODevice {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x40006000;
    static constexpr uint32_t DEFAULT_SPAN = 0x1000;

    // CR bits
    static constexpr uint32_t CR_EN      = (1u << 0);
    static constexpr uint32_t CR_TCIE    = (1u << 1);
    static constexpr uint32_t CR_SRC_INC = (1u << 2);
    static constexpr uint32_t CR_DST_INC = (1u << 3);
    // bits [5:4]: transfer size (0 = Byte, 1 = HalfWord, 2 = Word)

    // SR bits
    static constexpr uint32_t SR_BUSY = (1u << 0);
    static constexpr uint32_t SR_TCIF = (1u << 1);
    static constexpr uint32_t SR_TEIF = (1u << 2);
    static constexpr uint32_t SR_OVR  = (1u << 3);

    explicit DmaDevice(MemoryBus& bus, uint32_t base = DEFAULT_BASE, uint32_t span = DEFAULT_SPAN)
        : bus_(bus), base_(base), span_(span) {}

    [[nodiscard]] uint32_t base_address() const noexcept override { return base_; }
    [[nodiscard]] uint32_t address_span() const noexcept override { return span_; }
    [[nodiscard]] std::string device_name() const override { return "DMA0"; }
    [[nodiscard]] uint32_t irq_line() const noexcept override { return 3; }

    [[nodiscard]] uint32_t read(uint32_t offset, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x00) return src_;
        if (offset == 0x04) return dst_;
        if (offset == 0x08) return cndtr_;
        if (offset == 0x0C) return cr_;
        if (offset == 0x10) {
            uint32_t st = sr_;
            if (active_) st |= SR_BUSY;
            else st &= ~SR_BUSY;
            if (ovr_) st |= SR_OVR;
            return st;
        }
        if (offset == 0x14) return clkdiv_;
        return 0;
    }

    void write(uint32_t offset, uint32_t value, [[maybe_unused]] AccessSize size) override {
        if (offset == 0x10) {
            // Write-1-to-clear TCIF (bit 1), TEIF (bit 2), OVR (bit 3)
            if (value & SR_TCIF) sr_ &= ~SR_TCIF;
            if (value & SR_TEIF) sr_ &= ~SR_TEIF;
            if (value & SR_OVR)  ovr_ = false;
            return;
        }

        if (active_) {
            // Writing configuration/address registers while DMA is actively transferring triggers OVR
            if (offset == 0x00 || offset == 0x04 || offset == 0x08 || offset == 0x0C) {
                ovr_ = true;
                return;
            }
        }

        if (offset == 0x00) {
            src_ = value;
        } else if (offset == 0x04) {
            dst_ = value;
        } else if (offset == 0x08) {
            cndtr_ = value;
        } else if (offset == 0x0C) {
            cr_ = value;
            if ((cr_ & CR_EN) != 0) {
                if (cndtr_ == 0) {
                    cr_ &= ~CR_EN;
                    sr_ |= SR_TCIF;
                } else if (clkdiv_ == 0) {
                    // Fast 0-cycle burst mode: transfer all items immediately
                    while (cndtr_ > 0 && ((cr_ & CR_EN) != 0)) {
                        transfer_one_beat();
                    }
                } else {
                    active_ = true;
                    beat_counter_ = 0;
                }
            } else {
                active_ = false;
            }
        } else if (offset == 0x14) {
            clkdiv_ = value;
        }
    }

    void tick(uint64_t cycles) override {
        if (!active_ || (cr_ & CR_EN) == 0 || cndtr_ == 0) return;

        uint64_t step_ticks = (clkdiv_ == 0) ? 1ULL : static_cast<uint64_t>(clkdiv_);
        beat_counter_ += cycles;
        uint64_t beats = beat_counter_ / step_ticks;
        if (beats == 0) return;
        beat_counter_ %= step_ticks;

        for (uint64_t i = 0; i < beats && active_; ++i) {
            transfer_one_beat();
        }
    }

    void reset() override {
        src_ = 0;
        dst_ = 0;
        cndtr_ = 0;
        cr_ = 0;
        sr_ = 0;
        clkdiv_ = 0;
        beat_counter_ = 0;
        active_ = false;
        ovr_ = false;
    }

    [[nodiscard]] bool has_pending_irq() const noexcept override {
        return ((sr_ & SR_TCIF) != 0) && ((cr_ & CR_TCIE) != 0);
    }

private:
    void transfer_one_beat() {
        uint32_t size_code = (cr_ >> 4) & 0x03u;
        uint32_t step_bytes = (size_code == 0) ? 1u : (size_code == 1 ? 2u : 4u);

        try {
            if (step_bytes == 1) {
                uint8_t val = bus_.read8(src_);
                bus_.write8(dst_, val);
            } else if (step_bytes == 2) {
                uint16_t val = bus_.read16(src_);
                bus_.write16(dst_, val);
            } else {
                uint32_t val = bus_.read32(src_);
                bus_.write32(dst_, val);
            }
        } catch (const MemoryFaultException&) {
            sr_ |= SR_TEIF;
            active_ = false;
            cr_ &= ~CR_EN;
            return;
        }

        if (cr_ & CR_SRC_INC) src_ += step_bytes;
        if (cr_ & CR_DST_INC) dst_ += step_bytes;
        cndtr_--;

        if (cndtr_ == 0) {
            active_ = false;
            cr_ &= ~CR_EN;
            sr_ |= SR_TCIF;
        }
    }

    MemoryBus& bus_;
    uint32_t base_;
    uint32_t span_;
    uint32_t src_{0};
    uint32_t dst_{0};
    uint32_t cndtr_{0};
    uint32_t cr_{0};
    uint32_t sr_{0};
    uint32_t clkdiv_{0};
    uint64_t beat_counter_{0};
    bool active_{false};
    bool ovr_{false};
};

} // namespace tinyarmsim::mmio
