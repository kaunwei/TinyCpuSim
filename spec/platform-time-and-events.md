# Shared Platform Simulation Time & Event Scheduling Contract

## Problem Statement

Previously in TinyCpuSim, execution was driven by monolithic instruction stepping where simulated CPU steps were conflated with hardware cycles. In multi-subsystem, peripheral, and co-simulation setups, users and firmware developers could not determine whether CPU, timer, UART, DMA, and accelerator interactions represented a consistent simulated timeline. 

Because peripherals either advanced during CPU instructions or in arbitrary batch quantum loops, time-dependent behavior differed when changing batching sizes. Peripherals had no unified way to advance autonomously while CPUs waited in WFI (Wait-For-Interrupt) sleep, leading to artificial polling or lost causality in simulation.

## Solution

A unified, deterministic functional simulation time and event scheduling contract governed by `TimeScheduler`:
1. **Authoritative Functional Timeline**: Simulation time is represented as a monotonic 64-bit unsigned integer in abstract functional ticks starting at 0 (`now()`). One successful interpreter step costs exactly one tick.
2. **Deterministic Two-Phase Event Dispatch**: At each timestamp boundary, events are executed in strict priority phases:
   - `EventPhase::Device`: Autonomous device progression (e.g. Timer ticks, UART clock division, DMA transfer beats).
   - `EventPhase::Irq`: Interrupt publication and delivery to interrupt controllers (`NVICDevice`).
3. **Peripheral Time Adapters (`TimerAdapter`, `UartAdapter`, `DmaAdapter`)**: Modular adapters that connect standard MMIO devices to the `TimeScheduler` without polluting core device models, supporting fractional clock ratios via `ClockConversion` with remainder retention.
4. **WFI (Wait-For-Interrupt) Suspension & Autonomous Progression**: When CPUs enter WFI, the scheduler advances time directly to the earliest pending device/IRQ event without charging idle time as CPU progress.
5. **Cross-Platform Quantum Insensitivity**: All platforms (`BasicPerfPlatform`, `BasicMcuPlatform`, `McuConvPlatform`) conform to identical deadline execution entrypoints (`run_until(deadline, quantum)` and `run(max_steps, quantum)`), producing identical architectural and peripheral results regardless of whether quantum is 1, 8, 64, or 500.

## User Stories

1. As a platform developer, I want an authoritative 64-bit integer simulation timeline, so that CPU and peripheral interactions follow a well-defined causal order.
2. As a firmware developer, I want the CPU in WFI to suspend execution until a hardware interrupt arrives, so that idle wait loops do not consume fictional instruction progress.
3. As an SoC peripheral designer, I want autonomous devices (Timer, UART, DMA) to progress their internal state machines while the CPU is in WFI sleep, so that real-time hardware behavior is accurately simulated.
4. As an embedded software engineer, I want UART transmission to take time proportional to the baud rate clock divider, so that transmission delays and buffer filling match hardware expectations.
5. As a driver author, I want DMA transfers to transfer data beat-by-beat across the system bus and assert an interrupt upon completion, so that background memory transfers proceed autonomously without CPU polling.
6. As a multi-core platform architect, I want heterogeneous CPUs (Host MCU and Convolution Accelerator) to co-simulate under the same scheduler timeline, so that cross-core doorbells and mailbox protocols execute deterministically.
7. As a test author, I want execution results to remain identical across different scheduler quantum values (1, 8, 64, 500), so that simulation batching optimizations never alter architectural correctness.
8. As a simulation engineer, I want fractional clock ratios with remainder retention across ticks, so that non-integer frequency ratios between CPU and peripherals remain mathematically exact over long runs.
9. As a platform maintainer, I want strict input validation rejecting zero quantum and past deadlines, so that simulators fail fast on invalid arguments rather than deadlocking.
10. As an automated regression system, I want end-to-end multi-platform integration tests asserting deterministic timestamps and identical output hashes across all platforms, so that timing regressions are caught immediately.

## Implementation Decisions

1. **`TimeScheduler` Core Architecture**:
   - Maintains an ordered event queue sorted by `(time, phase, sequence)` where `EventPhase::Device < EventPhase::Irq`.
   - Event scheduling returns an idempotent cancellation `Handle`. Cancelled events are discarded safely without invalidating pointers.
   - Bounded same-time execution: protects against infinite loops by capping same-timestamp dispatches to 10,000 iterations.
   - Bounded execution loop `run(deadline, quantum, step_fn)` executes the user callback up to the deadline or until `StepResult::Wait`, `Halt`, or `Fault`.

2. **Clock Conversion Model (`ClockConversion`)**:
   - Converts input units to output ticks using floor division while accumulating fractional remainder between calls: `rem += input * num; output = rem / den; rem %= den`.
   - Checks intermediate multiplication and addition to prevent 64-bit overflow.

3. **`TimerAdapter` Design**:
   - Schedules periodic `EventPhase::Device` events advancing `TimerDevice::tick(converted_ticks)`.
   - Publishes `NVICDevice::set_pending(irq)` in `EventPhase::Irq` if `has_pending_irq()` is true.
   - Supports idempotent `stop()`, `reset()`, and destruction safety.

4. **`UartAdapter` Design**:
   - Tracks transmission activity via `UartDevice::is_busy()`.
   - Advances transmission in `EventPhase::Device` by `10 * clkdiv` ticks per frame.
   - Checks device reset generation counter to invalidate stale scheduled events upon UART reset.

5. **`DmaAdapter` Design**:
   - Inspects `(dma_.read(0x10) & DmaDevice::SR_BUSY) != 0` to detect active memory transfers.
   - Advances beat transfers in `EventPhase::Device` via `dma_.tick(1)`.
   - Publishes completion interrupt (IRQ 3) to `NVICDevice` in `EventPhase::Irq`.
   - Integrated into `BasicMcuPlatform` at `DMA_BASE` (`0x40006000`), exposed via `dma()`.

6. **Platform Uniform Conformance**:
   - `BasicPerfPlatform`, `BasicMcuPlatform`, and `McuConvPlatform` all implement:
     - `RunResult run_until(uint64_t deadline, uint64_t quantum = 500)`
     - `uint64_t run(uint64_t max_steps, uint64_t quantum = 500)`
     - `uint64_t time() const noexcept`
   - All three reject `quantum == 0` and `deadline < time()` with `std::invalid_argument`.
   - All three check deadline arithmetic overflow with `std::overflow_error`.

## Testing Decisions

1. **Clean-room Unit Testing (`tests/platform_time_test.cpp`)**:
   - Tests `TimeScheduler` event ordering, phase precedence, same-time event chaining, and cancellation.
   - Tests `TimerAdapter`, `UartAdapter`, and `DmaAdapter` independently with mock/real peripherals and NVIC.
   - Tests WFI wake-up behavior with masked/unmasked interrupts under ARM priority rules.
2. **Platform Conformance Testing**:
   - `PlatformTime.McuDmaTransfersDuringWfiWait`: Verifies DMA autonomously copies memory and wakes CPU from WFI.
   - `PlatformTime.DeterministicTimingConformanceAcrossAllThreePlatforms`: End-to-end test running all three platforms across quantum sweeps {1, 8, 64, 500}, asserting strict bitwise and timestamp parity.
3. **Zero Drift & Regression Invariants**:
   - Microarchitectural performance counters calibrated against golden reference (`tests/uarch/golden_counters.json`) with zero drift (<1% threshold).

## Out of Scope

- Hardware cycle-accurate pipeline simulation for simple microcontroller peripherals.
- Multithreaded parallel host execution backend (deferred to future Ticket 09).
- Dynamic power or voltage domain modeling.

## Further Notes

All platform timing conformance code has been verified and passes 100% across all 380 CTest targets. Full reference documentation is maintained in `docs/platform-time-reference.md`.
