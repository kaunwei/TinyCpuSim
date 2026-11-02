# Bare-Metal Firmware Development & Interactive Verification Framework

## Problem Statement

Developing bare-metal firmware for custom simulated platforms often suffers from lack of standardized board support packages (BSP), inconsistent interrupt management, and unpredictable timing in unit tests. Without standard bare-metal primitives (ring buffers, cooperative schedulers, non-blocking shells) and automated watchdog harnesses, testing firmware interactions with UART, timers, and interrupts required either ad-hoc mock scripts or manual inspection of console logs. Furthermore, debugging interactive firmware sessions in virtual platforms lacked remote execution control (breakpoints, single-stepping, frozen-time memory inspection).

## Solution

A production-grade bare-metal ARM Cortex-M3 firmware environment and verification framework:
1. **Zero-Allocation Bare-Metal BSP**:
   - Fixed-capacity compile-time circular ring buffer (`firmware/bsp/ring_buffer.h`) with power-of-2 wrap arithmetic.
   - Deterministic cooperative task scheduler (`firmware/bsp/scheduler.h`) supporting multiple periodic callbacks without dynamic memory allocation (`malloc`/heap).
   - Non-blocking interactive command shell (`firmware/bsp/shell.h`) handling line editing, command tokens, and extensible command dispatch (`ping`, `help`, `status`, `selfcheck`, `compute`).
2. **Standard Firmware Binaries & Applications**:
   - `firmware/template/`: Minimal starting point for custom user firmware verifying compiler options (`-O0`, `-O1`, `-O2`).
   - `firmware/apps/reference_app.c`: Full reference firmware featuring multi-frequency timer callbacks (1ms, 10ms, 100ms, 1000ms heartbeat), live UART RX/TX, and self-checks.
3. **Host Launcher & Debugging Protocols**:
   - Unified `scripts/run_firmware.sh` launcher supporting automated building, headless regression testing, and interactive console sessions via pseudo-terminal (`PTY`).
   - Remote JSON-over-socket control debugging protocol (`--control-fd`) supporting breakpoints (`break <addr>`, `break clear`), step-by-step execution (`step`, `continue`), CPU register inspection (`regs`), memory dumps (`mem <addr> <len>`), and guaranteed frozen-time simulation during pause.
4. **Automated Bounded Service Watchdog**:
   - `scripts/firmware_test_harness.py` providing `run_service_watchdog()` with explicit cycle budgets and wall-clock timeouts.
   - Output predicates that evaluate command response correctness decoupled from periodic heartbeats.

## User Stories

1. As a firmware developer, I want a zero-allocation ring buffer with compile-time capacities, so that interrupt service routines and shell drivers operate with zero risk of heap fragmentation or out-of-memory faults.
2. As an embedded engineer, I want a cooperative scheduler with fixed task capacity, so that periodic tasks (heartbeats, sensor polling, watchdog refreshing) run predictably at configured millisecond intervals.
3. As a platform developer, I want a non-blocking interactive command shell over UART, so that human operators and automated test harnesses can query status and invoke computation routines.
4. As a test engineer, I want an automated test harness with bounded step and wall-clock budgets, so that hanging firmware loops or deadlocks terminate with clean error traces rather than hanging CI pipelines.
5. As a developer debugging firmware faults, I want a socket-based control protocol supporting breakpoints and register/memory reads, so that I can inspect processor state at exact execution boundaries.
6. As a simulation engineer, I want paused debugging sessions to freeze simulation time completely, so that timers and peripheral state machines do not drift or accumulate spurious interrupts while inspecting memory.
7. As a firmware developer, I want firmware builds to succeed cleanly across optimization levels (`-O0`, `-O1`, `-O2`) with `-Wall -Werror`, so that compiler optimizations do not produce undefined behavior or register clobbering.
8. As a CI operator, I want standard CTest integration for all firmware targets, so that firmware regressions are continuously caught during normal simulator test runs.

## Implementation Decisions

1. **Bare-Metal Memory Layout & Vector Table**:
   - Linker script maps FLASH (ROM) starting at `0x00000000` and SRAM starting at `0x20000000`.
   - Vector table populates initial MSP stack pointer at SRAM top, `Reset_Handler`, `NMI_Handler`, `HardFault_Handler`, SysTick/Timer IRQ, and UART IRQ.
2. **Cooperative Scheduler Semantics**:
   - Tasks register a function pointer, period in ticks, and state flags.
   - `scheduler_tick()` decrements task counters; `scheduler_run()` executes ready tasks in priority order.
   - Schedulers run in foreground main loop without preemption, eliminating thread race conditions.
3. **UART Shell Architecture**:
   - Uses non-blocking character polling from peripheral RX registers.
   - Collects characters into a line buffer; handles backspace (`\b` / `0x7F`) and carriage return / line feed normalization (`\r\n`).
   - Tokenizes arguments with whitespace splitting and matches against static command registration table.
4. **Control Protocol & Debugger Harness**:
   - Communicates via Unix domain sockets or standard file descriptors using line-delimited JSON commands.
   - Synchronizes CPU step loop with socket event notifications.
   - Implements address breakpoint matching in the CPU interpreter fetch loop before instruction decoding.

## Testing Decisions

1. **Automated Watchdog Regression**:
   - Unit tests in `tests/firmware_test.py` execute reference firmware under `run_service_watchdog()`, verifying `[HB]`, `ping -> PONG`, `compute 5 -> 55`, and `selfcheck` outputs.
2. **Multi-Optimization Conformance**:
   - Firmware test suite compiles and runs test cases compiled under `-O0`, `-O1`, and `-O2`.
3. **Control Protocol Test Suite**:
   - Verified step, continue, breakpoint hits, and register queries against expected golden outputs.

## Out of Scope

- Preemptive real-time operating system (FreeRTOS / Zephyr) kernel scheduling.
- Dynamic memory management (`malloc` / `free`).
- Floating-point hardware context switching (FPU VFP).
