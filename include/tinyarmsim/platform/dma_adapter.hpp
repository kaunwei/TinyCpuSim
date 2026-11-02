#pragma once

#include <limits>
#include <memory>

#include "tinyarmsim/dev/dma.hpp"
#include "tinyarmsim/mmio/nvic.hpp"
#include "tinyarmsim/platform/time_scheduler.hpp"

namespace tinyarmsim::platform {

class DmaAdapter {
public:
    DmaAdapter(TimeScheduler& scheduler, mmio::DmaDevice& dma, mmio::NVICDevice& nvic,
               uint64_t numerator = 1, uint64_t denominator = 1)
        : scheduler_(scheduler), dma_(dma), nvic_(nvic), clock_(numerator, denominator) {}

    ~DmaAdapter() { stop(); }

    DmaAdapter(const DmaAdapter&) = delete;
    DmaAdapter& operator=(const DmaAdapter&) = delete;

    void stop() noexcept {
        scheduled_ = false;
        lifetime_.reset();
        device_event_.cancel();
        irq_event_.cancel();
    }

    void reset() {
        stop();
        clock_.reset();
        refresh();
    }

    void refresh() {
        bool busy = (dma_.read(0x10, mmio::AccessSize::Word) & mmio::DmaDevice::SR_BUSY) != 0;
        if (!busy) {
            device_event_.cancel();
            scheduled_ = false;
        } else if (!scheduled_) {
            if (!lifetime_) lifetime_ = std::make_shared<int>(0);
            schedule_next();
        }
    }

private:
    void schedule_next() {
        if (scheduler_.now() == std::numeric_limits<uint64_t>::max()) return;
        scheduled_ = true;
        std::weak_ptr<int> lifetime = lifetime_;
        device_event_ = scheduler_.schedule(scheduler_.now() + 1, EventPhase::Device, [this, lifetime] {
            if (lifetime.expired()) return;
            scheduled_ = false;
            dma_.tick(clock_.convert(1));
            irq_event_ = scheduler_.schedule(scheduler_.now(), EventPhase::Irq, [this, lifetime] {
                if (!lifetime.expired() && dma_.has_pending_irq()) nvic_.set_pending(dma_.irq_line());
            });
            if ((dma_.read(0x10, mmio::AccessSize::Word) & mmio::DmaDevice::SR_BUSY) != 0) schedule_next();
        });
    }

    TimeScheduler& scheduler_;
    mmio::DmaDevice& dma_;
    mmio::NVICDevice& nvic_;
    ClockConversion clock_;
    bool scheduled_ = false;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
    TimeScheduler::Handle device_event_, irq_event_;
};

} // namespace tinyarmsim::platform
