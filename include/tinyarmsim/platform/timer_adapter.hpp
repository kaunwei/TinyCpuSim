#pragma once
#include "tinyarmsim/dev/timer.hpp"
#include "tinyarmsim/mmio/nvic.hpp"
#include "tinyarmsim/platform/time_scheduler.hpp"
namespace tinyarmsim::platform {
// Adapter, timer and NVIC must outlive scheduled callbacks.
class TimerAdapter {
public:
    TimerAdapter(TimeScheduler& scheduler, mmio::TimerDevice& timer, mmio::NVICDevice& nvic,
                 uint64_t numerator = 1, uint64_t denominator = 1)
        : scheduler_(scheduler), timer_(timer), nvic_(nvic), clock_(numerator, denominator) { schedule_next(); }
    TimerAdapter(const TimerAdapter&) = delete;
    TimerAdapter& operator=(const TimerAdapter&) = delete;
private:
    void schedule_next() {
        if (scheduler_.now() == std::numeric_limits<uint64_t>::max()) return;
        scheduler_.schedule(scheduler_.now() + 1, EventPhase::Device, [this] {
            timer_.tick(clock_.convert(1));
            scheduler_.schedule(scheduler_.now(), EventPhase::Irq, [this] {
                if (timer_.has_pending_irq()) nvic_.set_pending(timer_.irq_line());
            });
            schedule_next();
        });
    }
    TimeScheduler& scheduler_;
    mmio::TimerDevice& timer_;
    mmio::NVICDevice& nvic_;
    ClockConversion clock_;
};
}
