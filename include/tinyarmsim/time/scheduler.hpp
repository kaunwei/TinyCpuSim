#pragma once
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <queue>
#include <stdexcept>
#include <tuple>
#include <vector>
#include "tinyarmsim/time/types.hpp"

namespace tinyarmsim::time {

class TimeScheduler {
public:
    using Handle = tinyarmsim::time::Handle;

    Handle schedule(uint64_t time, EventPhase phase, std::function<void()> callback) {
        if (time < now_ || (phase_started_ && time == now_ && phase < phase_)) {
            throw std::invalid_argument("event in the past or an earlier phase");
        }
        if (sequence_ == std::numeric_limits<uint64_t>::max()) {
            throw std::overflow_error("event sequence overflow");
        }
        auto state = std::make_shared<bool>(true);
        events_.push(Event{time, phase, sequence_++, std::move(callback), state});
        return Handle(state);
    }

    RunResult run(uint64_t deadline, uint64_t quantum, std::function<StepResult()> step) {
        if (quantum == 0) {
            throw std::invalid_argument("zero quantum");
        }
        if (deadline < now_) {
            throw std::invalid_argument("past deadline");
        }
        if (failed_ && deadline > now_) {
            throw std::runtime_error("scheduler stopped after callback failure");
        }
        uint64_t progress = 0;
        auto dispatch = [&] {
            size_t dispatched = 0;
            while (!events_.empty() && events_.top().time == now_) {
                if (!*events_.top().active) {
                    events_.pop();
                    continue;
                }
                if (++dispatched > 10000) {
                    throw std::runtime_error("nonprogressing event chain");
                }
                Event event = events_.top();
                events_.pop();
                *event.active = false;
                phase_started_ = true;
                phase_ = event.phase;
                event.callback();
            }
        };
        for (;;) {
            dispatch();
            if (now_ == deadline) {
                break;
            }
            const auto result = step();
            if (result == StepResult::Wait) {
                while (!events_.empty() && !*events_.top().active) {
                    events_.pop();
                }
                if (events_.empty() || events_.top().time > deadline) {
                    return {progress, StopReason::Wait};
                }
                now_ = events_.top().time;
                phase_started_ = false;
                continue;
            }
            if (result != StepResult::Progress) {
                return {progress, result == StepResult::Halt ? StopReason::Halt : StopReason::Fault};
            }
            try {
                dispatch();
            } catch (...) {
                failed_ = true;
                throw;
            }
            ++now_;
            phase_started_ = false;
            ++progress;
        }
        return {progress, StopReason::Deadline};
    }

    [[nodiscard]] uint64_t now() const noexcept { return now_; }

private:
    struct Event {
        uint64_t time;
        EventPhase phase;
        uint64_t sequence;
        std::function<void()> callback;
        std::shared_ptr<bool> active;
    };

    struct Later {
        bool operator()(const Event& a, const Event& b) const {
            return std::tie(a.time, a.phase, a.sequence) > std::tie(b.time, b.phase, b.sequence);
        }
    };

    std::priority_queue<Event, std::vector<Event>, Later> events_;
    uint64_t sequence_ = 0;
    bool phase_started_ = false;
    EventPhase phase_ = EventPhase::Device;
    uint64_t now_ = 0;
    bool failed_ = false;
};

} // namespace tinyarmsim::time
