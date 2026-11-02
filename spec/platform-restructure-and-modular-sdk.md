# Modular Platform Architecture & Public SDK Specification

## Problem Statement

Earlier versions of TinyCpuSim suffered from monolithic coupling between the CPU simulation core, SoC peripherals, and specific platform implementations. All headers resided in a flat `include/tinyarmsim/` namespace, and platform binaries were intertwined with core engine compilation. 

Furthermore, users could not develop external platforms without building the entire simulator internal test tree, and build/test commands were artificially restricted to half CPU capacity (`HALF_CORES`), unnecessarily slowing down developer turnaround.

## Solution

A modular, layered platform architecture and clean SDK export model:
1. **Categorized Header Hierarchy (Expand-Migrate-Contract)**:
   - `tinyarmsim/cpu/`: CPU core state, interpreter, decoder, disassembler, opcode cache, faults, NVIC.
   - `tinyarmsim/memory/`: Memory bus, loaders, address space translation.
   - `tinyarmsim/dev/`: Standard SoC peripherals (Timer, UART, GPIO, DMA, SPI, I2C, CRC32).
   - `tinyarmsim/platform/`: Timing scheduler, event adapters, virtual platform orchestrator, accelerator subsystems.
   - `tinyarmsim/utils/`: CLI options, tracing, firmware helpers, signal handling.
   - Legacy flat includes (`include/tinyarmsim/*.hpp`) forward cleanly to new paths for backward compatibility.
2. **Three Independent Platform Projects**:
   - `platforms/01_basic_perf`: High-throughput performance benchmark platform.
   - `platforms/02_basic_mcu`: Embedded microcontroller platform with UART, Timer, GPIO, and DMA peripherals.
   - `platforms/03_coprocessor_accel`: Dual-core heterogeneous co-simulation platform with Mailbox and 2D Convolution Accelerator.
   - Each platform maintains its own CMake target, topology descriptor, tests, firmware, and demo executable.
3. **Precompiled Static Core Engine & Public SDK Export (`sdk/`)**:
   - `libtinyarmsim_core_static.a` encapsulates the complete simulation engine.
   - Platform projects consume the core library without re-compiling simulator sources.
   - The CMake build targets automatically package public headers and static libraries into `sdk/include/` and `sdk/lib/`.
4. **Max-Parallelism Build & Test Infrastructure**:
   - Removed CPU throttling; utilized full CPU core capacity (`-j$(nproc)`) across CMake, Ninja, and CTest.
5. **Interactive 2-Step Launcher (`scripts/run_platform.sh`)**:
   - Step 1: Select Platform (`01_basic_perf`, `02_basic_mcu`, `03_coprocessor_accel`).
   - Step 2: Select Action (Run Demo, Run Tests, Build Firmware).

## User Stories

1. As an external simulator developer, I want a cleanly packaged `sdk/` containing only public headers and `libtinyarmsim_core_static.a`, so that I can create custom virtual platforms without compiling internal simulator tests.
2. As a platform author, I want modular platform directories (`platforms/XX/`) with dedicated `topology.hpp`, so that custom platform wiring does not pollute the simulator core.
3. As a developer, I want categorized headers (`tinyarmsim/cpu/`, `tinyarmsim/dev/`, `tinyarmsim/platform/`), so that include dependencies are structured, navigable, and adhere to clean modular boundaries.
4. As a continuous integration engineer, I want CMake and CTest to utilize all available CPU cores (`-j$(nproc)`), so that regression testing finishes in seconds rather than minutes.
5. As a new user, I want an interactive launcher `scripts/run_platform.sh`, so that I can explore demos and execute platform-specific tests without memorizing complex CMake build paths.
6. As an embedded firmware engineer, I want platform-specific firmware (`firmware/`) isolated within its own platform directory, so that compiling firmware for one MCU does not require rebuilding another platform.

## Implementation Decisions

1. **Header Reorganization**:
   - Organized core headers into subdirectories (`cpu/`, `memory/`, `dev/`, `platform/`, `utils/`).
   - Top-level legacy header forwarding (`include/tinyarmsim/*.hpp -> include/tinyarmsim/*/*.hpp`) maintained without breaking external consumers.
2. **Core Static Library (`tinyarmsim_core_static`)**:
   - Compiles core CPU, uarch, memory bus, and peripheral classes once into `libtinyarmsim_core_static.a`.
   - All platform demo binaries and test executables link against `tinyarmsim_core_static`.
3. **SDK Packaging Rule**:
   - Root `CMakeLists.txt` defines custom command `package_sdk` which copies public headers to `sdk/include/` and static libraries to `sdk/lib/`.
4. **Platform Restructure**:
   - `platforms/01_basic_perf`: provides `BasicPerfPlatform` with workload loaders and cycle-count estimation.
   - `platforms/02_basic_mcu`: provides `BasicMcuPlatform` with NVIC, Timer, UART, GPIO, and DMA.
   - `platforms/03_coprocessor_accel`: provides `McuConvPlatform` with 2D Convolution Accelerator Subsystem and Mailbox.

## Testing Decisions

1. **Independent Platform Test Suites**:
   - `platforms/01_basic_perf/tests/test_perf_platform.cpp`
   - `platforms/02_basic_mcu/tests/test_mcu_platform.cpp`
   - `platforms/03_coprocessor_accel/tests/test_coprocessor_accel.cpp`
2. **SDK Standalone Compilation Verification**:
   - Verifies that external test snippets link against `sdk/lib/libtinyarmsim_core_static.a` without access to simulator internal source directories.
3. **Zero Drift Invariants**:
   - Microarchitectural performance counters maintain <1% error margin against empirical gem5 golden references.

## Out of Scope

- Dynamic runtime plugin loading via `.so`/`.dll` (static library linkage chosen for zero-overhead performance and deterministic symbol resolution).
- GUI configuration visualizer for platform wiring.

## Further Notes

All three platform demo binaries (`platform_01_perf_demo`, `platform_02_mcu_demo`, `platform_03_accel_demo`) build and run cleanly via `scripts/run_platform.sh`.
