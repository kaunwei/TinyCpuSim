# TinyCpuSim Architecture & Subsystem Specifications

This directory contains formal, authoritative specifications for TinyCpuSim subsystems and architectural contracts. All specifications follow the `/to-spec` standard (Problem Statement, Solution, User Stories, Implementation Decisions, Testing Decisions, Out of Scope).

## Active Subsystem Specifications

| Specification | Document | Description | Status |
| :--- | :--- | :--- | :--- |
| **Flexible Virtual Platform** | [`spec/flexible-virtual-platform.md`](flexible-virtual-platform.md) | Composable dual-core virtual platform architecture, heterogeneous coprocessor doorbells, and matrix convolution acceleration. | Verified & Complete |
| **Modular Platform Architecture & SDK** | [`spec/platform-restructure-and-modular-sdk.md`](platform-restructure-and-modular-sdk.md) | Reorganized header hierarchy, standalone CMake platform targets, public SDK directory (`sdk/`), and multi-platform interactive launcher. | Verified & Complete |
| **Platform Simulation Time & Events** | [`spec/platform-time-and-events.md`](platform-time-and-events.md) | Authoritative 64-bit integer simulation timeline (`TimeScheduler`), two-phase event queue (`Device` vs `Irq`), peripheral adapters (`TimerAdapter`, `UartAdapter`, `DmaAdapter`), single-ownership invariant, and parallel feasibility evaluation. | Verified & Complete |
| **Bare-Metal Firmware & Verification** | [`spec/bare-metal-firmware-framework.md`](bare-metal-firmware-framework.md) | Zero-allocation bare-metal BSP (ring buffer, cooperative scheduler, interactive shell), reference apps, JSON control protocol debugging, and bounded watchdog test harness. | Verified & Complete |
| **Standalone Simulation Time Library** | [`spec/standalone-simulation-time-library.md`](standalone-simulation-time-library.md) | Decoupled discrete-event simulation time library (`tinyarmsim_time`), modular namespace `tinyarmsim::time`, and clean-room SDK export for future virtual platforms. | Active / Ready for Implementation |
