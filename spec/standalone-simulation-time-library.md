# Standalone Simulation Time & Discrete-Event Scheduling Library

## Problem Statement

Currently in TinyCpuSim, the simulation time scheduler (`TimeScheduler`), clock arithmetic (`ClockConversion`), and event adapters (`TimerAdapter`, `UartAdapter`, `DmaAdapter`) are located within `include/tinyarmsim/platform/` alongside high-level platform topologies and virtual CPU instance definitions. While they are functionally decoupled from specific CPU ISAs, future virtual platform targets (such as RISC-V co-simulators, DSP accelerator boards, or multi-cluster SMP prototypes) cannot currently reuse this deterministic timing engine without pulling in broader CPU and MMIO dependencies. 

Platform developers and hardware engineers need a lean, standalone C++20 discrete-event scheduling library (`tinyarmsim_time`) that can be used independently to orchestrate multi-core coordination, autonomous device progression, and deterministic time advancement with zero external simulator coupling.

## Solution

Extract the core simulation time, fractional clock conversion, and event dispatch mechanics into an independent, reusable library component:
1. **Modular Namespace & Directory Hierarchy**: Establish `tinyarmsim::time` in `include/tinyarmsim/time/`:
   - `types.hpp`: Discrete event phases (`EventPhase::Device`, `EventPhase::Irq`), execution progress statuses (`StepResult`), termination causes (`StopReason`), cancellation tokens (`Handle`), and execution results (`RunResult`).
   - `clock.hpp`: Standalone `ClockConversion` arithmetic retaining remainders and preventing 64-bit overflow.
   - `scheduler.hpp`: Core single-thread deterministic priority-queue discrete event scheduler (`TimeScheduler`), independent of CPU or bus models.
   - `adapters/`: Generic peripheral time adapters (`timer_adapter.hpp`, `uart_adapter.hpp`, `dma_adapter.hpp`).
2. **Dedicated Standalone CMake Target**: Provide `tinyarmsim_time` as a first-class CMake static/interface target and package it into `sdk/include/tinyarmsim/time/` and `sdk/lib/`.
3. **Backward-Compatible Seams**: Retain type aliases in `include/tinyarmsim/platform/time_scheduler.hpp` (`using TimeScheduler = tinyarmsim::time::TimeScheduler;`) to ensure existing platforms and tests compile without disruption during migration.
4. **Zero-Coupling Clean-Room Verification**: Implement standalone library test cases verifying that `tinyarmsim::time` compiles and executes in an isolated translation unit without including any CPU, bus, or MMIO headers.

## User Stories

1. As a virtual platform architect, I want a standalone discrete-event scheduling library, so that I can construct new simulation platforms without pulling in ARM-specific CPU or MMIO headers.
2. As an embedded simulation developer, I want `TimeScheduler` to depend strictly on standard C++20 headers (`<cstdint>`, `<functional>`, `<limits>`, `<memory>`, `<queue>`), so that the library can be integrated into external simulation harnesses.
3. As an SoC peripheral designer, I want `ClockConversion` to live in its own header, so that I can compute fractional frequency dividers with remainder retention without instantiating an entire event scheduler.
4. As a test author, I want event cancellation handles (`Handle`) to be lightweight, movable, and idempotent, so that peripheral teardown and reset routines can discard scheduled callbacks safely.
5. As an engine developer, I want `TimeScheduler::run()` to operate against a generic `std::function<StepResult()>` callback, so that heterogeneous compute engines (ARM interpreter, RISC-V interpreter, FFT pipeline, matrix convolution engine) can be co-scheduled with identical causality semantics.
6. As a multi-core architect, I want `EventPhase::Device` and `EventPhase::Irq` to maintain strict priority precedence at identical timestamps, so that peripheral state transitions always settle before interrupts are sampled.
7. As an SDK consumer, I want `tinyarmsim_time` to be available as an exported CMake target in `sdk/lib/`, so that standalone platform projects can link it directly via `target_link_libraries(my_platform PRIVATE TinyCpuSim::Time)`.
8. As a legacy platform maintainer, I want `tinyarmsim::platform::TimeScheduler` to remain valid as a type alias, so that existing platforms (`BasicPerfPlatform`, `BasicMcuPlatform`, `McuConvPlatform`) compile cleanly without breaking changes.
9. As a library maintainer, I want clean-room unit tests that compile against `tinyarmsim/time/scheduler.hpp` in complete isolation, so that accidental header leakage or hidden dependencies are trapped at compile time.
10. As a performance engineer, I want the event queue dispatch to operate with near-zero overhead (< 50 ns per event), so that high-frequency timer ticks and UART clock steps do not degrade whole-system simulation throughput.

## Implementation Decisions

1. **Namespace & File Separation**:
   - Establish namespace `tinyarmsim::time`.
   - Separate primitives: `types.hpp` (enums, structs, `Handle`), `clock.hpp` (`ClockConversion`), `scheduler.hpp` (`TimeScheduler`).
   - Move peripheral time adapters into `include/tinyarmsim/time/adapters/` while preserving compatibility wrappers in `include/tinyarmsim/platform/`.
2. **Library Packaging in CMake**:
   - Define CMake target `tinyarmsim_time` with public include directory `${CMAKE_CURRENT_SOURCE_DIR}/include`.
   - Export alias `TinyCpuSim::Time`.
   - Link `tinyarmsim_time` into `tinyarmsim_core_static` so existing consumers automatically receive the headers.
   - Install public headers to `sdk/include/tinyarmsim/time/`.
3. **Expand-Migrate-Contract Progression**:
   - Phase 1 (Expand): Create new headers under `include/tinyarmsim/time/` and declare CMake target `tinyarmsim_time`. Provide type aliases in `include/tinyarmsim/platform/`.
   - Phase 2 (Migrate): Migrate platform adapters (`TimerAdapter`, `UartAdapter`, `DmaAdapter`) and platforms (`01_basic_perf`, `02_basic_mcu`, `03_coprocessor_accel`) to consume `tinyarmsim::time`.
   - Phase 3 (Contract): Retire redundant internal declarations, keeping only clean backward-compatibility forwards.
4. **Header Independence Guarantee**:
   - `tinyarmsim/time/*.hpp` headers must never include `tinyarmsim/cpu/*`, `tinyarmsim/memory/*`, or `tinyarmsim/mmio/*`.

## Testing Decisions

1. **Highest Seam Behavioral Testing**:
   - Exercise scheduling ordering, two-phase resolution (`Device` before `Irq`), remainder retention, same-time dispatch capping, and cancellation through public `TimeScheduler` methods.
2. **Clean-Room Compilation Seam (`tests/test_standalone_time_lib.cpp`)**:
   - A dedicated unit test translation unit that includes ONLY `#include "tinyarmsim/time/scheduler.hpp"` and `<gtest/gtest.h>`, proving complete decoupling from core simulation components.
3. **Multi-Platform Quantum Conformance Regression**:
   - Re-run existing `PlatformTime.DeterministicTimingConformanceAcrossAllThreePlatforms` to ensure zero functional or timing drift across all platforms.

## Out of Scope

- Multi-threaded or distributed parallel event queue implementations (per Ticket 09 NO-GO evaluation).
- Hardware cycle-accurate timing simulation or physical delay modeling.
- External SystemC / TLM-2.0 bridge interfaces.

## Further Notes

This extraction deepens the architecture by transforming the platform timing subsystem into a reusable component of the TinyCpuSim SDK, enabling future platform archetypes to be constructed rapidly with proven timing invariants.
