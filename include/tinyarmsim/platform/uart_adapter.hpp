#pragma once
#include "tinyarmsim/dev/uart.hpp"
#include "tinyarmsim/mmio/nvic.hpp"
#include "tinyarmsim/platform/time_scheduler.hpp"

namespace tinyarmsim::platform {
// UART and NVIC outlive this adapter; the scheduler may be destroyed first.
class UartAdapter {
public:
    UartAdapter(TimeScheduler& scheduler, mmio::UartDevice& uart, mmio::NVICDevice& nvic,
                uint64_t numerator = 1, uint64_t denominator = 1)
        : scheduler_(scheduler), uart_(uart), nvic_(nvic), clock_(numerator, denominator),
          generation_(uart.reset_generation()) {}
    ~UartAdapter() { stop(); }
    UartAdapter(const UartAdapter&) = delete;
    UartAdapter& operator=(const UartAdapter&) = delete;
    void stop() noexcept {
        lifetime_.reset();
        device_event_.cancel();
        irq_event_.cancel();
        scheduled_ = false;
    }
    void refresh() {
        if (generation_ != uart_.reset_generation()) {
            stop();
            clock_.reset();
            generation_ = uart_.reset_generation();
        }
        if (!uart_.is_busy()) {
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
        const auto generation = generation_;
        device_event_ = scheduler_.schedule(scheduler_.now() + 1, EventPhase::Device, [this, lifetime, generation] {
            if (lifetime.expired()) return;
            scheduled_ = false;
            if (generation != uart_.reset_generation()) { refresh(); return; }
            uart_.tick(clock_.convert(1));
            irq_event_ = scheduler_.schedule(scheduler_.now(), EventPhase::Irq, [this, lifetime, generation] {
                if (!lifetime.expired() && generation == uart_.reset_generation() && uart_.has_pending_irq())
                    nvic_.set_pending(uart_.irq_line());
            });
            if (uart_.is_busy()) schedule_next();
        });
    }
    TimeScheduler& scheduler_;
    mmio::UartDevice& uart_;
    mmio::NVICDevice& nvic_;
    ClockConversion clock_;
    uint64_t generation_;
    bool scheduled_ = false;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
    TimeScheduler::Handle device_event_, irq_event_;
};
}
