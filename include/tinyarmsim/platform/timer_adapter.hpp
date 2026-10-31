#pragma once
#include "tinyarmsim/dev/timer.hpp"
#include "tinyarmsim/mmio/nvic.hpp"
#include "tinyarmsim/platform/time_scheduler.hpp"
namespace tinyarmsim::platform {
// The timer and NVIC must outlive the adapter.
class TimerAdapter {
public:
    TimerAdapter(TimeScheduler& scheduler, mmio::TimerDevice& timer, mmio::NVICDevice& nvic,
                 uint64_t numerator = 1, uint64_t denominator = 1, bool stop_when_disabled = false)
        : scheduler_(scheduler), timer_(timer), nvic_(nvic), clock_(numerator, denominator),
          stop_when_disabled_(stop_when_disabled) { schedule_next(); }
    ~TimerAdapter() { stop(); }
    void stop() noexcept {
        running_ = false;
        lifetime_.reset();
        device_event_.cancel();
        irq_event_.cancel();
    }
    void reset() {
        stop();
        clock_.reset();
        lifetime_ = std::make_shared<int>(0);
        schedule_next();
    }
    void start() { if (!running_) reset(); }
    TimerAdapter(const TimerAdapter&) = delete;
    TimerAdapter& operator=(const TimerAdapter&) = delete;
private:
    void schedule_next() {
        if (scheduler_.now() == std::numeric_limits<uint64_t>::max()) return;
        running_ = true;
        std::weak_ptr<int> lifetime = lifetime_;
        device_event_ = scheduler_.schedule(scheduler_.now() + 1, EventPhase::Device, [this, lifetime] {
            if (lifetime.expired()) return;
            timer_.tick(clock_.convert(1));
            irq_event_ = scheduler_.schedule(scheduler_.now(), EventPhase::Irq, [this, lifetime] {
                if (lifetime.expired()) return;
                if (timer_.has_pending_irq()) nvic_.set_pending(timer_.irq_line());
            });
            if (!stop_when_disabled_ || enabled()) schedule_next();
            else running_ = false;
        });
    }
    bool enabled() {
        return (timer_.read(8, mmio::AccessSize::Word) & 1u) != 0 &&
               timer_.read(0, mmio::AccessSize::Word) != 0;
    }
    TimeScheduler& scheduler_;
    mmio::TimerDevice& timer_;
    mmio::NVICDevice& nvic_;
    ClockConversion clock_;
    bool stop_when_disabled_;
    bool running_ = false;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
    TimeScheduler::Handle device_event_, irq_event_;
};
}
