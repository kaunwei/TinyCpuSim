# Standalone Time Library Architecture (`tinyarmsim::time`)

The `tinyarmsim::time` library provides a standalone, zero-dependency, single-host-thread discrete-event simulation kernel and fractional clock arithmetic foundation for TinyCpuSim. Decoupled completely from CPU instruction sets, memory buses, and hardware registers, the library can be used independently or embedded within arbitrary platform simulators.

---

## 1. Architectural Overview & Library Targets

The library is designed as a pure C++20 header-only modular component located in `include/tinyarmsim/time/`. It depends exclusively on the C++ standard library (`<cstdint>`, `<functional>`, `<limits>`, `<memory>`, `<queue>`, `<stdexcept>`, `<tuple>`, `<vector>`).

### CMake Target

The library is exported as an interface target:
- **CMake Target**: `tinyarmsim_time`
- **Alias**: `TinyCpuSim::Time`
- **Include Path**: `include/`

```cmake
# Consuming tinyarmsim::time in external or platform targets
find_package(TinyCpuSim REQUIRED)
target_link_libraries(my_custom_platform PRIVATE TinyCpuSim::Time)
```

### Component Structure

```
include/tinyarmsim/time/
├── types.hpp              # Fundamental types, enums, run results, and cancellation handles
├── clock.hpp              # Fractional clock conversion with remainder preservation
├── scheduler.hpp          # Deterministic two-phase discrete-event priority scheduler
└── adapters/              # Reusable peripheral timing adapters
    ├── timer_adapter.hpp  # Periodic timer tick and interrupt scheduler
    ├── uart_adapter.hpp   # Asymmetric baud rate and FIFO transmission scheduler
    └── dma_adapter.hpp    # Burst/beat DMA transfer and interrupt scheduler
```

---

## 2. Core Types & Scheduler Semantics

### Core Types (`tinyarmsim/time/types.hpp`)

- `EventPhase`: Two-phase scheduling enum (`Device = 0`, `Irq = 1`).
- `StepResult`: Status returned by participant step invocations:
  - `Progress`: Successfully advanced execution by one step/instruction.
  - `Wait`: Suspended execution waiting on external event / interrupt (e.g., WFI).
  - `Halt`: Explicit terminal state reached (e.g., breakpoint or software interrupt).
  - `Fault`: Unrecoverable CPU or hardware fault.
- `StopReason`: Termination condition reported by scheduler `run()`:
  - `Deadline`: Absolute target timestamp reached.
  - `Wait`: Execution suspended in wait state with no scheduled events through deadline.
  - `Halt`: Participant requested halt.
  - `Fault`: Execution faulted.
- `RunResult`: Contains `uint64_t progress` (actual successful steps executed) and `StopReason reason`.
- `Handle`: Copyable, thread-safe cancellation handle wrapping `std::weak_ptr<bool>`.
  - `cancel()` is idempotent and marks the scheduled event as inactive.
  - An inactive event is discarded upon reaching the top of the event priority queue without executing.
  - Calling `cancel()` on an empty or expired handle is a safe no-op.

### Priority Queue Event Ordering

Events are stored in `std::priority_queue` with a strict deterministic comparator `Later`:
```cpp
bool operator()(const Event& a, const Event& b) const {
    return std::tie(a.time, a.phase, a.sequence) > std::tie(b.time, b.phase, b.sequence);
}
```
1. **Timestamp (`time`)**: Earliest absolute functional tick executes first.
2. **Phase (`phase`)**: `EventPhase::Device` strictly precedes `EventPhase::Irq` at the same tick.
3. **Sequence (`sequence`)**: Monotonically incrementing 64-bit integer ensures FIFO dispatch for events scheduled at the identical timestamp and phase.

### Execution Loop (`TimeScheduler::run`)

```cpp
RunResult run(uint64_t deadline, uint64_t quantum, std::function<StepResult()> step);
```

1. **Dispatch at Current Time**: All active events scheduled at `now()` are dispatched in phase and sequence order before invoking `step()`.
2. **Deadline Check**: If `now() == deadline`, the scheduler returns `{progress, StopReason::Deadline}`.
3. **Step Invocation**: `step()` executes a single participant action.
   - If `StepResult::Wait`: The scheduler scans pending events. If no active events exist up to `deadline`, it returns `{progress, StopReason::Wait}`. If a future event exists at $T \le \text{deadline}$, time fast-forwards directly to $T$ without charging idle ticks to CPU progress.
   - If `StepResult::Halt` or `StepResult::Fault`: Returns immediately with the corresponding stop reason.
   - If `StepResult::Progress`: Events scheduled during `step()` at the current timestamp are dispatched, time advances by 1 (`++now_`), and `progress` increments.
4. **Non-Progress Chain Guard**: More than 10,000 events dispatched at the same timestamp triggers `std::runtime_error("nonprogressing event chain")`.
5. **Exception Safety & Fail-Stop**: If an event callback throws an exception, remaining events at that timestamp are retained. The scheduler enters a fail-stop state that prevents replaying the committed step at the same timestamp.

---

## 3. ClockConversion Fractional Arithmetic (`tinyarmsim/time/clock.hpp`)

Different simulation components (CPUs, bus clocks, timers, UART bauds) often operate at non-integer frequency ratios. `ClockConversion` implements exact rational scaling with remainder accumulation, eliminating clock drift over long simulation runs.

### Mathematical Formulation

Given a frequency ratio $\frac{N}{D}$ (where $N = \text{numerator}$, $D = \text{denominator}$):
$$\text{total} = \text{input} \times N + \text{remainder}$$
$$\text{output} = \lfloor \frac{\text{total}}{D} \rfloor$$
$$\text{remainder}' = \text{total} \pmod D$$

```cpp
class ClockConversion {
public:
    ClockConversion(uint64_t numerator, uint64_t denominator);
    uint64_t convert(uint64_t input);
    void reset() noexcept;
};
```

### Key Properties

- **Drift-Free Conversion**: Fractional remainders are accumulated in `remainder_` and rolled into subsequent conversion cycles.
- **Strict Overflow Guard**: Before multiplication, it verifies:
  $$\text{input} \le \frac{\text{UINT64\_MAX} - \text{remainder}}{N}$$
  If input exceeds this threshold, `std::overflow_error` is thrown and `remainder_` is left unchanged.
- **Ratio Validation**: Initializing with zero numerator or zero denominator throws `std::invalid_argument`.
- **Reset Invariant**: `reset()` clears `remainder_ = 0` when resetting a device or clock tree.

---

## 4. Two-Phase Event Phases

In discrete-event simulation of microcontrollers and SoCs, simultaneous events within a single simulation tick can produce race conditions if peripheral state updates and interrupt signals interleave arbitrarily.

`tinyarmsim::time` enforces a strict two-phase ordering within each timestamp:

```
Timestamp T:
  ┌──────────────────────────────────────────────┐
  │ Phase 0: EventPhase::Device                  │
  │   - Advance peripheral hardware counters     │
  │   - Perform DMA beats / UART state transfers │
  │   - Update device internal status flags      │
  └──────────────────────┬───────────────────────┘
                         ▼
  ┌──────────────────────────────────────────────┐
  │ Phase 1: EventPhase::Irq                     │
  │   - Evaluate interrupt line conditions       │
  │   - Assert / publish IRQs to NVIC            │
  │   - Unpend / wake CPU wait states            │
  └──────────────────────┬───────────────────────┘
                         ▼
                   [CPU Step]
```

### Phase Boundary Invariants

1. **Precedence**: All `EventPhase::Device` events at tick $T$ run before any `EventPhase::Irq` event at tick $T$.
2. **Phase Monotonicity**: Once dispatch begins for `EventPhase::Irq` at tick $T$, scheduling a new event for `EventPhase::Device` at tick $T$ is strictly forbidden and throws `std::invalid_argument("event in the past or an earlier phase")`.
3. **IRQ Delivery Before Resumption**: Device state changes that occur at tick $T$ post their interrupt assertions to `EventPhase::Irq` at tick $T$, guaranteeing that the CPU's subsequent step observes the interrupt before executing instructions at tick $T+1$.

---

## 5. Custom Platform Integration Guide

To build a custom simulation platform using `tinyarmsim::time`:

### Step 1: Instantiate the TimeScheduler

```cpp
#include "tinyarmsim/time/scheduler.hpp"
#include "tinyarmsim/time/clock.hpp"

class MyCustomPlatform {
public:
    MyCustomPlatform() : scheduler_() {}

    tinyarmsim::time::RunResult run_until(uint64_t deadline, uint64_t quantum = 1) {
        return scheduler_.run(deadline, quantum, [this] {
            return step_cpu();
        });
    }

    uint64_t time() const noexcept { return scheduler_.now(); }

private:
    tinyarmsim::time::StepResult step_cpu() {
        if (cpu_in_wfi_) {
            return tinyarmsim::time::StepResult::Wait;
        }
        if (cpu_halted_) {
            return tinyarmsim::time::StepResult::Halt;
        }
        try {
            cpu_.execute_one_instruction();
            return tinyarmsim::time::StepResult::Progress;
        } catch (const CpuFaultException&) {
            return tinyarmsim::time::StepResult::Fault;
        }
    }

    tinyarmsim::time::TimeScheduler scheduler_;
    // CPU core and peripheral members...
};
```

### Step 2: Implement Peripheral Timing Adapters

Wrap asynchronous peripherals with adapter objects that schedule events on the `TimeScheduler`.

#### Pattern: Lifetime Guarding & Two-Phase Scheduling

```cpp
class CustomPeripheralAdapter {
public:
    CustomPeripheralAdapter(tinyarmsim::time::TimeScheduler& scheduler,
                            MyPeripheral& dev,
                            MyInterruptController& ic,
                            uint64_t clock_num = 1,
                            uint64_t clock_den = 1)
        : scheduler_(scheduler), dev_(dev), ic_(ic), clock_(clock_num, clock_den) {
        schedule_next();
    }

    ~CustomPeripheralAdapter() { stop(); }

    void stop() noexcept {
        running_ = false;
        lifetime_.reset();
        dev_handle_.cancel();
        irq_handle_.cancel();
    }

    void reset() {
        stop();
        clock_.reset();
        lifetime_ = std::make_shared<int>(0);
        schedule_next();
    }

private:
    void schedule_next() {
        if (scheduler_.now() == std::numeric_limits<uint64_t>::max()) return;
        running_ = true;
        std::weak_ptr<int> lifetime = lifetime_;

        // Phase 1: Device tick
        dev_handle_ = scheduler_.schedule(scheduler_.now() + 1,
                                          tinyarmsim::time::EventPhase::Device,
                                          [this, lifetime] {
            if (lifetime.expired()) return;
            dev_.tick(clock_.convert(1));

            // Phase 2: Signal IRQ in Irq phase at the same timestamp
            irq_handle_ = scheduler_.schedule(scheduler_.now(),
                                              tinyarmsim::time::EventPhase::Irq,
                                              [this, lifetime] {
                if (lifetime.expired()) return;
                if (dev_.has_pending_irq()) {
                    ic_.assert_irq(dev_.irq_line());
                }
            });

            if (dev_.is_active()) {
                schedule_next();
            } else {
                running_ = false;
            }
        });
    }

    tinyarmsim::time::TimeScheduler& scheduler_;
    MyPeripheral& dev_;
    MyInterruptController& ic_;
    tinyarmsim::time::ClockConversion clock_;
    bool running_ = false;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
    tinyarmsim::time::Handle dev_handle_, irq_handle_;
};
```

### Step 3: Reusing Built-in Adapters

`tinyarmsim::time` includes production-ready adapters under `tinyarmsim::time::adapters`:
- `TimerAdapter`: Drives `tinyarmsim::mmio::TimerDevice` and publishes pending interrupts to `tinyarmsim::mmio::NVICDevice`.
- `UartAdapter`: Handles rate-scaled UART transmit/receive delays and FIFO interrupt notifications.
- `DmaAdapter`: Orchestrates burst DMA memory-to-memory transfers and completion interrupts during CPU compute and WFI idle states.
