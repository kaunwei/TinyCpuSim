#pragma once
#include <cstdint>
#include <functional>
#include "tinyarmsim/time/types.hpp"
#include "tinyarmsim/time/clock.hpp"
#include "tinyarmsim/time/scheduler.hpp"
#include "tinyarmsim/interpreter.hpp"

namespace tinyarmsim::platform {

using EventPhase = tinyarmsim::time::EventPhase;
using StepResult = tinyarmsim::time::StepResult;
using StopReason = tinyarmsim::time::StopReason;
using RunResult = tinyarmsim::time::RunResult;
using Handle = tinyarmsim::time::Handle;
using ClockConversion = tinyarmsim::time::ClockConversion;

class TimeScheduler : public tinyarmsim::time::TimeScheduler {
public:
    using tinyarmsim::time::TimeScheduler::TimeScheduler;

    RunResult run_interpreter(uint64_t deadline, uint64_t quantum, IsaInterpreter& interpreter) {
        return run(deadline, quantum, [&] {
            if (interpreter.waiting()) return StepResult::Wait;
            try { interpreter.step(); return StepResult::Progress; }
            catch (const CpuFaultException& fault) {
                return fault.get_fault_type() == FaultType::SoftwareInterrupt ? StepResult::Halt : StepResult::Fault;
            }
        });
    }
};

} // namespace tinyarmsim::platform
