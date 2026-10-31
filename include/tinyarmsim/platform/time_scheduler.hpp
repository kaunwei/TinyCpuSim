#pragma once
#include <cstdint>
#include <stdexcept>
#include <functional>
#include <queue>
#include <limits>
#include <tuple>
#include "tinyarmsim/interpreter.hpp"

namespace tinyarmsim::platform {
class ClockConversion {
public:
    ClockConversion(uint64_t numerator, uint64_t denominator) : numerator_(numerator), denominator_(denominator) {
        if (!numerator || !denominator) throw std::invalid_argument("zero clock ratio");
    }
    uint64_t convert(uint64_t input) {
        constexpr auto max = std::numeric_limits<uint64_t>::max();
        if (input > (max - remainder_) / numerator_) throw std::overflow_error("clock conversion overflow");
        const auto total = input * numerator_ + remainder_;
        remainder_ = total % denominator_;
        return total / denominator_;
    }
private:
    uint64_t numerator_, denominator_, remainder_ = 0;
};
enum class EventPhase { Device, Irq };
enum class StepResult { Progress, Halt, Fault };
enum class StopReason { Deadline, Halt, Fault };
struct RunResult { uint64_t progress; StopReason reason; };
class TimeScheduler {
public:
    void schedule(uint64_t time, EventPhase phase, std::function<void()> callback) {
        if (time < now_ || (phase_started_ && time == now_ && phase < phase_))
            throw std::invalid_argument("event in the past or an earlier phase");
        if (sequence_ == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("event sequence overflow");
        events_.push(Event{time, phase, sequence_++, std::move(callback)});
    }
    template<class Step> RunResult run(uint64_t deadline, uint64_t quantum, Step step) {
        if (quantum == 0) throw std::invalid_argument("zero quantum");
        if (deadline < now_) throw std::invalid_argument("past deadline");
        uint64_t progress = 0;
        for (;;) {
            size_t dispatched = 0;
            while (!events_.empty() && events_.top().time == now_) {
                if (++dispatched > 10000) throw std::runtime_error("nonprogressing event chain");
                Event event = events_.top(); events_.pop();
                phase_started_ = true; phase_ = event.phase;
                event.callback();
            }
            if (now_ == deadline) break;
            const auto result = step();
            if (result != StepResult::Progress)
                return {progress, result == StepResult::Halt ? StopReason::Halt : StopReason::Fault};
            ++now_;
            phase_started_ = false;
            ++progress;
        }
        return {progress, StopReason::Deadline};
    }
    RunResult run_interpreter(uint64_t deadline, uint64_t quantum, IsaInterpreter& interpreter) {
        return run(deadline, quantum, [&] {
            try { interpreter.step(); return StepResult::Progress; }
            catch (const CpuFaultException& fault) {
                return fault.get_fault_type() == FaultType::SoftwareInterrupt ? StepResult::Halt : StepResult::Fault;
            }
        });
    }
    uint64_t now() const { return now_; }
private:
    struct Event { uint64_t time; EventPhase phase; uint64_t sequence; std::function<void()> callback; };
    struct Later { bool operator()(const Event& a, const Event& b) const {
        return std::tie(a.time, a.phase, a.sequence) > std::tie(b.time, b.phase, b.sequence);
    }};
    std::priority_queue<Event, std::vector<Event>, Later> events_;
    uint64_t sequence_ = 0;
    bool phase_started_ = false;
    EventPhase phase_ = EventPhase::Device;
    uint64_t now_ = 0;
};
}
