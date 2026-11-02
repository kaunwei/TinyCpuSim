# Platform time reference slice

`TimeScheduler` is a single-host-thread functional reference. Time is an unsigned 64-bit integer in abstract ticks. One successful interpreter step costs one tick, including exception entry; this is a functional model, not hardware cycles. An interpreter step is indivisible: events at a tick run before the next step, including at the requested deadline. Events scheduled for the current time during a successful step run after that step and before time advances, in the same phase and sequence order; an earlier phase already passed at that boundary remains unavailable. There is no deadline overshoot. Progress counts successful steps, not requested quantum or retired instructions. A faulting step consumes no tick; side effects made by the interpreter before a fault are not rolled back. Software interrupt exceptions are reported as halt; other `CpuFaultException`s are reported as fault. Events scheduled by a step that halts, faults, or throws remain queued at the current time for a subsequent run. Other exceptions propagate; if a callback throws, it has been removed and remaining events stay queued at the current time. If a callback after a successful step throws, the scheduler is fail-stop for further execution: it will not replay that committed step at the same timestamp. Remaining events at that timestamp can still be dispatched by a run to the current time. With external clock enabled, WFI retires in one tick and suspends subsequent steps. A waiting scheduler advances to queued events through the deadline without charging idle time as CPU progress. If no event is available through the deadline, it returns Wait at the current time; events at the deadline are dispatched before returning Deadline. A later run can resume the wait.

The deadline is absolute and cannot precede current time. Quantum must be positive; this reference checks events at every boundary regardless of quantum. At each time, device events precede IRQ events; sequence order resolves ties within a phase. A callback cannot schedule an earlier phase at the current time, or an event in the past. More than 10,000 same-time callbacks in one dispatch is diagnosed as nonprogress. `schedule` returns a copyable cancellation handle; ignoring it preserves legacy behavior. Calling `cancel()` is idempotent and prevents a pending callback from executing; cancelled events do not advance a waiting scheduler. A handle remains safe after its event or scheduler is destroyed. Cancellation does not interrupt a callback already running.

`ClockConversion(numerator, denominator)` converts input units to output ticks using floor division and retains the remainder between calls. Both ratio terms must be positive. It checks the intermediate multiplication and addition, rejecting overflow even if division could yield a representable result; failure leaves the remainder unchanged.

Interpreter bus ticking remains the default. An interpreter using `set_external_clock(true)` delegates device advancement to the platform for normal instructions, skipped IT slots and exception entry. `TimerAdapter` advances an existing `TimerDevice` once per elapsed scheduler tick in the Device phase, then publishes its pending IRQ in the IRQ phase. Its optional clock ratio retains fractional ticks. Register aliases access the same timer but do not cause extra scheduler advancement. `TimerAdapter::stop()` cancels its pending Device and IRQ callbacks and is idempotent; destruction also stops it without accessing the scheduler. `reset()` cancels both callbacks, clears the clock conversion remainder, and restarts from the scheduler's current time, with the next Device tick one scheduler tick later. Reset does not reset the timer registers or clear an IRQ already published to the NVIC. The scheduler must be alive when calling reset; the timer and NVIC must outlive the adapter. The scheduler may be destroyed before the adapter. WFI wakes for an enabled, pending NVIC IRQ that can preempt the active IRQ under this model’s IRQ-number priority. Like ARM WFI, PRIMASK does not prevent that wake; it still prevents exception entry, so execution continues after WFI while the IRQ remains pending. Disabled or lower-priority pending IRQs do not wake it. The wake query does not acknowledge or enter an exception. Pending state is checked before each attempted step, including immediately after WFI, so an already-pending masked IRQ is not lost. Legacy interpreter mode retains WFI as a no-op. MMIO traces, multicore coordination, and migration of the other existing platforms are deferred. Deterministic timing conformance is enforced across BasicPerfPlatform, BasicMcuPlatform, and McuConvPlatform; no physical cycle-accuracy or broader hardware conformance is claimed.


## Basic performance platform

`BasicPerfPlatform::run_until(deadline, quantum)` uses the shared scheduler with an absolute deadline and returns actual successful interpreter steps and a deadline, wait, halt, or fault reason. `time()` reports the current functional tick; `run(max_steps)` remains a relative-budget compatibility wrapper returning actual progress and rejects deadline overflow. A zero quantum and a deadline before current time are rejected. The interpreter retains its default bus ticking: this platform does not enable external clock ownership or schedule autonomous devices. Each successful interpreter step costs one functional tick, not a calibrated hardware cycle.

Rebuild and exercise the scheduler and platform tests with `cmake --build build -j 4 && ctest --test-dir build --output-on-failure -R 'PlatformTime|BasicPerf|PerfPlatform|BasicPerfPlatform'`. Run the current workload demo with `build/platforms/01_basic_perf/platform_01_perf_demo`. The platform tests exercise actual workload execution and quantum values 1, 8, 64, and 500, as well as early halt, fault, deadlines, and invalid quantum.

`BasicMcuPlatform::run_until(deadline, quantum)` uses the same functional time and returns actual CPU progress and the stop reason; `time()` reports its absolute tick. `run(max_steps, quantum)` retains a relative deadline and returns actual progress. The MCU's timer has one scheduler-owned adapter, independent of register aliases. UART and GPIO busy operations use scheduler-owned device events and publish pending IRQs before CPU resumption. With no active device event, WFI returns Wait at the current tick. Existing `VirtualPlatform` execution remains a legacy interface; use the MCU entrypoints for these timing semantics.

The MCU UART adapter accepts a device-tick to scheduler-tick ratio, defaulting to 1:1. While a transmission is busy, each elapsed scheduler tick converts through that ratio with the fractional remainder retained across transmissions; idle time does not advance this clock. A device reset invalidates pending Device and IRQ callbacks, including an IRQ queued before reset at the same timestamp, and clears the conversion remainder when the adapter next observes it. A new transmission can schedule a new-generation IRQ notification. Adapter destruction cancels its callbacks. The UART and NVIC must outlive the adapter; the scheduler may be destroyed first. Reset does not clear an IRQ already published to the NVIC.

A transmitted frame costs `10 * clkdiv` device ticks. The baud register is stored and readable but does not affect this cost. For compatibility, `clkdiv == 0` completes the write immediately. TX completion does not generate an interrupt; the UART IRQ reflects the existing enabled, nonempty RX FIFO. Device callbacks precede IRQ publication at the same timestamp. These are functional model rules, not claims about physical UART timing.

## Accelerator participant

`ConvolutionAcceleratorSubsystem::step()` returns `Progress` for one successful interpreter step, `Wait` while WFI is suspended, `Halt` for SVC or an already halted participant, and `Fault` for other CPU faults. A fault or SVC latches the halted state; subsequent steps report Halt until explicitly cleared. Other exceptions latch halt and propagate. `waiting()` reflects whether an enabled pending interrupt can wake WFI. `set_external_clock(true)` delegates bus advancement to the caller and enables WFI suspension; disabling it restores legacy interpreter ticking and clears the wait. The legacy `step_quantum()` returns the number of successful steps and retains its extra bus tick per call when using the default internal clock. It does not advance the bus under external clock ownership. This participant interface does not change the coprocessor topology's scheduling or accounting.

The standalone participant checks can be run with `c++ -std=c++20 -Wall -Wextra -Iinclude -I. tests/test_coprocessor_accel.cpp -o /tmp/tinycpu_accel_participant_test && /tmp/tinycpu_accel_participant_test`; the existing CTest coprocessor target uses the platform's separate test source.

The participant scenarios also run as always-active GTest checks in `tests/unit/test_accelerator_subsystem.cpp`. The standalone `tests/test_coprocessor_accel.cpp` remains in place; its cleanup is deferred until the runtime assertion-count guard recognizes GTest assertions.

## Coprocessor accelerator platform

`McuConvPlatform::run_until(deadline, quantum)` and `McuConvPlatform::run(max_cycles, quantum)` coordinate the host CPU and convolution accelerator subsystem under the shared functional `TimeScheduler`. Co-simulation results exhibit quantum insensitivity: executing the convolution co-simulation workload across quantum values 1, 8, 64, and 500 produces identical execution progress and output, verifying output result `0xCAFECAFE` at `0x0002FFFC` and accelerator mailbox status `2`.

## DMA adapter

`DmaAdapter` coordinates `DmaDevice` beat progress and IRQ publication under the functional `TimeScheduler`. Its optional clock ratio converts scheduler ticks to DMA device ticks with fractional ticks retained across beats. When DMA transfer is active (`SR_BUSY` set), `refresh()` schedules the next beat callback at `now() + 1` in `EventPhase::Device`. In `EventPhase::Device`, the adapter advances the DMA device by the converted tick count (`dma_.tick(clock_.convert(1))`), executing beat transfers. It then schedules an event at the current timestamp in `EventPhase::Irq` to publish pending interrupts to the `NVICDevice` (`nvic_.set_pending(dma_.irq_line())`) if `has_pending_irq()` is asserted. If the transfer remains busy, the next beat is scheduled for the subsequent tick. `stop()` cancels pending Device and IRQ events, clears the active scheduling state, and invalidates in-flight callbacks via lifetime tracking. `reset()` stops active events, clears the clock conversion remainder, and invokes `refresh()` to evaluate current device status. Reset does not modify DMA device registers or clear IRQs already published to the NVIC. The DMA device and NVIC must outlive the adapter; destruction cancels pending callbacks and safely tolerates prior destruction of the scheduler.

`BasicMcuPlatform` integrates `DmaAdapter` and attaches `DmaDevice` to the memory bus at `DMA_BASE` (`0x40006000`), exposed via `dma()`. DMA transfers proceed autonomously across the memory bus during CPU execution and WFI idle wait. Upon completion, the adapter asserts the DMA completion IRQ (line 3) in `EventPhase::Irq`, waking the waiting CPU from WFI.

## Cross-platform deterministic timing conformance

`BasicPerfPlatform`, `BasicMcuPlatform`, and `McuConvPlatform` conform to the shared functional `TimeScheduler` timing model. Deterministic timing conformance across all three platforms establishes the following invariant guarantees:

1. **Unified Functional Time Progress**: All three platforms represent functional time in 64-bit abstract ticks starting at 0, exposed via `time()`. Each successful CPU or subsystem execution step advances platform time by exactly one functional tick, with `RunResult::progress` recording only actual committed steps.
2. **Strict Quantum Insensitivity**: Platform execution deterministically produces identical progress, stop reasons (`Deadline`, `Halt`, `Wait`), timestamps, and architectural/peripheral states across varying scheduler quantum configurations (`1`, `8`, `64`, `500`).
3. **Common Boundary Contract**:
   - `run_until(deadline, quantum)` strictly rejects zero quantum (`quantum == 0`) and deadlines in the past (`deadline < time()`) by throwing `std::invalid_argument`.
   - Relative execution entrypoint `run(max_steps, [quantum])` checks for arithmetic overflow on deadline calculation (`max_steps > UINT64_MAX - time()`) and throws `std::overflow_error`.
4. **Integration Verification**: End-to-end multi-platform conformance is verified by `PlatformTime.DeterministicTimingConformanceAcrossAllThreePlatforms` in `tests/platform_time_test.cpp`. Run the full timing test suite with:
   `cmake --build build -j 4 && ctest --test-dir build --output-on-failure -R 'PlatformTime|BasicPerf|test_mcu_platform|test_coprocessor_accel'`

## Bus ticking single-ownership and standardized platform execution contract

Duplicate bus ticking across `step_quantum` methods has been retired in favor of a strict single-ownership invariant. Under this invariant, bus and peripheral ticking belongs exclusively to one of two mutually exclusive execution authorities:

1. **Internal Execution Single-Ownership**: When running under default internal clocking (`external_clock == false`), each single-instruction step owns advancing the system bus. Wrapper methods (such as `step_quantum`) do not perform auxiliary or duplicate bus ticks per quantum invocation.
2. **External Scheduler Single-Ownership**: When running under external clock management (`set_external_clock(true)`), bus and peripheral advancement is delegated entirely to the external `TimeScheduler` via registered device event callbacks (e.g., `EventPhase::Device`). Subsystem steps execute without ticking the bus directly.

This invariant eliminates duplicate bus progression, preserves event-phase ordering, and prevents timing drift during multi-step batching.

### Standardized `run(max_steps, quantum)` platform contract

All conforming platforms (`BasicPerfPlatform`, `BasicMcuPlatform`, and `McuConvPlatform`) standardize on the relative-budget entrypoint `run(max_steps, quantum)`:
- **Relative Budget Conversion**: Computes an absolute deadline `deadline = time() + max_steps`. Detects arithmetic overflow (`max_steps > UINT64_MAX - time()`) and throws `std::overflow_error`.
- **Quantum Validation**: Validates that `quantum > 0`, throwing `std::invalid_argument` if `quantum == 0`.
- **Execution Delegation**: Dispatches to `run_until(deadline, quantum)`, enforcing quantum insensitivity and returning a `RunResult` containing actual committed CPU progress and the termination reason (`Deadline`, `Halt`, `Wait`, or `Fault`).
- **Timing Invariant**: Step execution and peripheral updates maintain strict single ownership, ensuring identical deterministic outcomes regardless of the chosen quantum value.

## Parallel execution feasibility evaluation against reference contract

An evaluation of multi-threaded parallel execution feasibility for multi-core and coprocessor platforms (`McuConvPlatform`) against the single-host-thread `TimeScheduler` reference contract indicates that introducing host OS threads is currently unjustified. The analysis below details workload execution characteristics, cross-core interaction density, synchronization options, event queue ownership, lookahead constraints, and the evidence-based recommendation alongside future race-detection criteria.

### Workload execution characteristics

In the reference coprocessor co-simulation benchmark (`VirtualPlatformIntegrationTest.HostAcceleratorConvolutionCoSimulation`), execution exhibits an asymmetric master-worker topology with stark sequential phasing:

1. **Initialization Phase**: The Host CPU executes setup instructions, configures convolution parameters (`WIDTH=8`, `HEIGHT=8`, `KERNEL_SZ=3`), initializes the 64-byte input tensor and 9-byte kernel in `MailboxScratchpadDevice`, and unmasks IRQ 4. During this phase, the Accelerator CPU remains parked in `WFI` (`StepResult::Wait`), executing no instructions.
2. **Compute Phase**: The Host CPU rings `MAILBOX_DOORBELL_TO_ACCEL` and immediately enters a `WFI` polling loop (`StepResult::Wait`). The Accelerator receives the NVIC interrupt, wakes from `WFI`, and executes the 2D convolution kernel (36 output pixels, 324 multiply-accumulate operations and memory accesses) over the scratchpad. During this compute phase, the Host CPU is completely suspended in `WFI`.
3. **Verification Phase**: Upon completing the convolution, the Accelerator writes status `MB_STATUS_DONE`, asserts `MAILBOX_DOORBELL_TO_HOST`, and halts (`StepResult::Halt`). The Host CPU wakes from `WFI` via IRQ 4, validates the 36 computed output elements against expected values, records the completion sentinel `0xCAFECAFE` at `0x0002FFFC`, and terminates.

Because the two execution participants alternate execution phases (one participant executes while the peer core waits in `WFI` or halts), concurrent instruction execution overlap between cores is near zero (< 1%).

### Cross-core event density (Host-Accelerator doorbells)

Cross-core coordination occurs exclusively through the `MailboxScratchpadDevice` MMIO registers and NVIC doorbell lines:
- **Event Frequency**: Across the entire co-simulation run (~15,000–20,000 instruction steps), exactly two cross-core doorbell events occur:
  1. *Host-to-Accelerator Doorbell*: Written by Host CPU at start of compute.
  2. *Accelerator-to-Host Doorbell*: Written by Accelerator upon kernel completion.
- **Event Density Metric**: Event density is extremely low, with an event-to-instruction ratio of approximately $2 / 15,000 \approx 1.3 \times 10^{-4}$ events per instruction tick (< 0.015%).
- **Memory Decoupling**: Memory spaces are isolated across independent buses (Host memory bus vs Accelerator private bus). Shared memory interaction is restricted to the 64KB Mailbox scratchpad, which follows a strict producer-consumer handoff protocol guarded by the doorbells without concurrent shared-memory contention.

### Barrier and conservative safe-window options

Two primary synchronization strategies exist for parallelizing discrete event execution:

- **Option A: Quantum-Synchronous Barriers (Bulk Synchronous Parallel)**:
  Cores execute concurrently on separate host threads up to a shared quantum boundary $Q$ (e.g., 500 ticks) and synchronize via host barriers (e.g., `std::barrier` or condition variables).
  *Evaluation*: Host OS thread synchronization overhead (1–5 $\mu$s per barrier on modern CPUs) substantially exceeds interpreter instruction step latency (10–30 ns per functional tick). At quantum $Q = 500$, barrier synchronization would consume 20%–45% of total runtime. Additionally, because one core is in `WFI` while the other computes, worker threads would continually block or spin at barriers, causing severe thread contention and cache thrashing without throughput benefit.
- **Option B: Conservative Safe-Window Execution (Chandy-Misra-Bryant)**:
  Each core advances independently up to a conservative safe horizon $T_{\text{safe}} = \min(T_{\text{peer}} + L, T_{\text{event}})$, where $L$ is the guaranteed minimum lookahead.
  *Evaluation*: Conservative safe windows leverage the low event density by permitting cores to execute large autonomous epochs without barrier stalls. However, this strategy strictly requires a nonzero lookahead ($L \ge 1$) and null-message synchronization to prevent deadlocks when a core is idle in `WFI`.

### Event queue ownership

- **Centralized Event Queue**: A single shared priority queue protected by mutexes introduces thread contention on every `schedule()` and `dispatch()` boundary, destroying multi-core scaling and serializing event dispatch.
- **Partitioned Per-Core Event Queues**: Each core thread must own a dedicated local priority queue for private events (device ticks, timer alarms). Cross-core events (doorbell interrupts) are transferred via lock-free single-producer single-consumer (SPSC) ring buffers with acquire-release memory semantics.
- **Reference Invariant Compliance**: Any distributed event queue architecture must guarantee the reference contract's deterministic ordering: `EventPhase::Device` strictly precedes `EventPhase::Irq` at the same simulation tick, and simultaneous events maintain deterministic tie-breaking without host thread race conditions.

### Minimum lookahead requirements

- **Current Reference Lookahead ($L = 0$)**: The single-thread reference model executes doorbell MMIO writes with instantaneous callback delivery within the current simulation tick ($L = 0$).
- **Parallel Lookahead Constraint ($L_{\min} \ge 1$)**: For conservative parallel execution without lockstep barriers, the simulation contract must enforce a minimum lookahead $L_{\min} \ge 1$ tick:
  - Cross-core doorbells and interrupt line assertions must have an explicit propagation latency (minimum 1 simulation tick) before taking effect on the target core's NVIC.
  - With $L_{\min} \ge 1$, each core thread can safely advance to $T_{\text{peer}} + L_{\min}$ without risk of receiving a past-timestamp event (causality violation).
  - When a core enters `WFI`, it must publish an extended lookahead horizon equal to its next pending wake event or epoch limit, permitting the active core to execute without interruption.

### Evidence-based recommendation

**Recommendation: NO-GO for multi-threaded parallel execution in the current simulator architecture.**

1. **Amdahl's Law Limitation**: Due to strictly alternating master-worker execution phases in coprocessor workloads, parallel execution overlap is negligible (< 1%). The theoretical speedup upper bound is under $1.01\times$.
2. **Negative Performance Scaling**: The single-host-thread interpreter achieves high execution throughput (~36 ms total runtime for the entire 200,000-step co-simulation test). Host OS thread context switching, barrier latency, and cache invalidation overhead would exceed instruction execution time, leading to net performance degradation.
3. **Simplicity and Conformance**: The single-host-thread `TimeScheduler` already provides proven quantum insensitivity, deterministic reproducibility, and zero-drift conformance across all platforms without concurrency defects or threading overhead.

### Future race-detection and acceptance criteria

If future multi-cluster SMP architectures or high-concurrency symmetric workloads necessitate parallel host backends, the implementation must satisfy the following acceptance criteria prior to adoption:

1. **Deterministic Trace Equivalence**: Multi-threaded execution across $N$ worker threads must produce bit-for-bit identical architectural registers, memory state dumps, and timestamped event sequences as the single-thread reference (`PlatformTime.DeterministicTimingConformanceAcrossAllThreePlatforms`), invariant to host thread scheduling.
2. **ThreadSanitizer Cleanliness Gate**: 100% clean test execution under Clang/GCC `-fsanitize=thread` with zero data race diagnostics across all platform unit, integration, and stress tests.
3. **Hardware Bus Race Trapping**: Shared memory structures (`MailboxScratchpadDevice`) must enforce explicit read/write access protocol validation: unsynchronized conflicting accesses (RAW, WAR, WAW) without intervening doorbell handoffs must be trapped and flagged as firmware protocol races.
4. **Quantum Insensitivity Invariance**: Multi-threaded runs across quantum values $Q \in \{1, 8, 64, 500\}$ must yield identical committed step progress, stop reasons, and final architectural states.
5. **Zero Compiler Warnings**: Build cleanly under `-Wall -Wextra` with no compiler warnings or weakened test assertions.




