# Firmware Development & Acceptance Guide

## Overview

TinyCpuSim provides a bare-metal ARM Cortex-M3 firmware environment supported by clocked MMIO peripherals, an interrupt controller (NVIC), and an interactive host debugging harness.

## Architecture & Components

1. **Minimal Template (`firmware/template/`)**:
   - Starting point for custom firmware.
   - Self-check routine testing configuration definitions (`fw_config.h`).
   - Supports `-O0`, `-O1`, `-O2` builds with zero compiler warnings.

2. **Full Reference Firmware (`firmware/apps/reference_app.c`)**:
   - Cooperative static scheduler (`firmware/bsp/scheduler.h`) with compile-time fixed capacities.
   - Power-of-2 circular ring buffer (`firmware/bsp/ring_buffer.h`) with zero dynamic allocation.
   - Multi-period timer callbacks: 1 ms, 10 ms, 100 ms, and 1000 ms periodic UART heartbeat (`[HB]\n`).
   - Interactive UART command shell (`firmware/bsp/shell.h`) with non-blocking character reception and deterministic commands:
     - `ping` -> `PONG`
     - `help` -> Command listing
     - `status` -> `STATUS: OK`
     - `selfcheck` -> Evaluates `FW_SELF_CHECK_EXPECTED`
     - `compute <N>` -> Deterministic sum of squares

3. **Host Launcher & Tools**:
   - `scripts/run_firmware.sh`: Unified launcher supporting `build`, `test`, `interactive` (with PTY), `--quiet`, and signal trap cleanup.
   - Top-level integration via `./run.sh fw`.

4. **Debugging Protocol (`--control-fd`)**:
   - JSON-over-socket control protocol.
   - Commands: `break <addr>`, `break clear`, `pause`, `step`, `continue`, `regs`, `mem <addr> <len>`, `stop`.
   - Frozen-time guarantee: paused simulator halts timer ticks and peripheral state evolution.

5. **Bounded Watchdog Contract**:
   - Automated test verification via `run_service_watchdog()` in `scripts/firmware_test_harness.py`.
   - Progress predicates verify target service outputs independent of heartbeats.
   - Explicit step budgets and wall-clock timeouts prevent runaway processes.
