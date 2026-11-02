#pragma once
#include <cstdint>
#include <memory>

namespace tinyarmsim::time {

enum class EventPhase { Device, Irq };
enum class StepResult { Progress, Wait, Halt, Fault };
enum class StopReason { Deadline, Wait, Halt, Fault };

struct RunResult {
    uint64_t progress;
    StopReason reason;
};

class Handle {
public:
    Handle() = default;
    explicit Handle(std::weak_ptr<bool> state) : state_(std::move(state)) {}
    explicit Handle(const std::shared_ptr<bool>& state) : state_(state) {}
    void cancel() const {
        if (auto state = state_.lock()) {
            *state = false;
        }
    }
private:
    std::weak_ptr<bool> state_;
};

} // namespace tinyarmsim::time
