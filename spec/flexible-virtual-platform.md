# Flexible Virtual Platform Architecture & Convolution Accelerator Subsystem

## Problem Statement

Currently, simulation in TinyCpuSim is centered around a single monolithic CPU and a static, flat system bus (`SoCPeripherals::attach_all`). Simulating multi-core clusters or heterogeneous multi-subsystem architectures—such as an MCU coupled to an intelligent peripheral module (e.g., an SSD controller or hardware accelerator that encapsulates its own private CPU and address space)—requires duplicating harness code or modifying the simulator core.

Furthermore, cache hierarchies are currently hard-wired inside the multi-core system rather than being modular components that can be composed arbitrarily. Developers cannot easily configure a single core with private L1 and a dedicated L2, or bind multiple cores to a shared coherent L2, without refactoring the core simulation engine.

## Solution

Build a composable **Flexible Virtual Platform Framework** featuring:
1. **Hierarchical Subsystem & Bus Encapsulation**: Each CPU or composite peripheral subsystem can own an independent address space (`MemoryBus`). Complex peripherals (such as accelerators or SSD controllers) can encapsulate their own private CPU, private bus, and local SRAM, exposing only a clean MMIO window to the host bus.
2. **Modular Cache Hierarchy Binding**: Decouple the CPU from outer-level caches. A CPU entity owns its private L1I/L1D, while shared or private L2 caches become first-class platform components wired via standardized memory interfaces.
3. **Phase-1 Heterogeneous Demonstration (Convolution Accelerator)**: Provide a dual-firmware bare-metal demo where a Host MCU commands a Convolution Accelerator Subsystem over a bidirectional Doorbell/Mailbox interface. The accelerator encapsulates an internal controller CPU running dedicated microcode to execute 2D convolution over internal scratchpad memory.
4. **Single-Page Topology DSL**: Enable declaring the entire platform topology (CPUs, clocks, buses, cache binding, and MMIO endpoints) in a concise C++ descriptor linking directly against the precompiled core static library.

## User Stories

1. As a virtual platform architect, I want to instantiate independent `MemoryBus` instances for distinct subsystems, so that peripheral and controller address spaces do not collide or leak into the host address space.
2. As a platform developer, I want to declare a composite subsystem that encapsulates an internal CPU and private bus, so that I can model intelligent devices (such as SSD controllers or coprocessors) cleanly.
3. As a firmware engineer, I want to load independent ELF binary images into separate CPUs across the virtual platform, so that the host application and accelerator microcode can be compiled, booted, and debugged independently.
4. As a microarchitectural researcher, I want to attach a CPU's L1 cache to a dedicated L2 cache for single-core evaluations, so that I can accurately characterize single-thread cache miss penalties.
5. As a microarchitectural researcher, I want to attach multiple CPU cores to a single shared L2 cache with MESI coherence, so that I can model symmetric multiprocessing (SMP) clusters without modifying core CPU code.
6. As a platform modeler, I want to configure different clock domain frequencies for different subsystems (e.g., a 100MHz MCU and a 1GHz accelerator), so that cross-subsystem timing ratios reflect realistic hardware environments.
7. As a simulator engineer, I want quantum-based co-simulation scheduling across all active CPUs, so that execution remains deterministic, fast, and easy to trace.
8. As a firmware developer, I want a bidirectional Mailbox and Doorbell MMIO interface between host and accelerator, so that tasks can be dispatched and signaled via hardware interrupts following Lean ISR principles.
9. As a platform designer, I want to define the entire platform interconnect topology in a single readable C++ descriptor, so that platform wiring can be audited at a glance on one screen.
10. As a performance engineer, I want the Convolution Accelerator to compute 2D matrix convolutions over internal scratchpad RAM, so that mathematical correctness and timing delay can be verified end-to-end.
11. As a CI/verification engineer, I want automated CTest integration verifying the complete dual-firmware handshake and mathematical convolution output, so that regressions are automatically prevented.

## Implementation Decisions

1. **Subsystem and Bus Isolation**:
   - Each platform entity (Host MCU, Accelerator) maintains its own `MemoryBus` instance.
   - Devices on the Host bus interact with sub-modules strictly via registered `MMIODevice` adapters (e.g., Mailbox CSRs and shared scratchpad aperture).
   - Local sub-module buses, private SRAMs, and local interrupt controllers are strictly unmapped from the host bus.

2. **Decoupled Cache Hierarchy**:
   - The CPU core provides an outward memory request interface (representing L1 miss traffic).
   - `L2Cache` is an independent composable class. In single-core configurations, a core's L1 miss port binds to a private L2 cache. In multi-core configurations, multiple core L1 ports bind to a shared L2 cache. If L2 is omitted, the L1 miss port connects directly to the target `MemoryBus`.

3. **Bidirectional Mailbox & Doorbell Interface**:
   - The communication bridge exposes MMIO registers at a defined host base address (e.g., `0x50000000`):
     - `PARAM_REG0` .. `PARAM_REG3` (image width, height, stride, kernel dimension)
     - `STATUS_REG` (IDLE, RUNNING, DONE, ERROR)
     - `DOORBELL_TO_ACCEL` (writing 1 asserts IRQ to internal CPU)
     - `DOORBELL_TO_HOST` (writing 1 asserts IRQ to Host NVIC)
     - `INTCLR` (clears pending doorbell interrupt)
     - Scratchpad memory aperture (input feature map, weight kernel, output buffer)

4. **Co-Simulation Quantum Dispatcher**:
   - The virtual platform orchestrator advances active cores in round-robin instruction quantums (e.g., 500–1000 steps).
   - Interrupt lines and doorbell signals updated during a quantum are asserted at quantum boundaries, ensuring deterministic execution.

5. **Convolution Accelerator Internal Architecture**:
   - Encapsulates an internal CPU (using the core library), private bus, private boot vector/SRAM, and local NVIC.
   - Bootstraps from `conv_accelerator.elf`.
   - On receiving a doorbell interrupt, reads parameters from the Mailbox, performs 2D convolution over the scratchpad array, writes the status code, and triggers the host doorbell.

6. **Host Application Architecture**:
   - Bootstraps from `conv_host.elf`.
   - Initializes host peripherals, writes test patterns into the accelerator scratchpad, rings the doorbell, waits on interrupt/WFI, and validates output against known mathematical invariants.

7. **Zero Core Recompilation**:
   - All platform wiring connects against the existing static library (`libtinyarmsim_core_static.a`).

## Testing Decisions

- **High Seam Testing**: Verification occurs primarily at the platform acceptance level (`tests/virtual_platform_test.cpp`) and bare-metal firmware execution level (`firmware/projects/conv_host` & `firmware/projects/conv_accelerator`).
- **End-to-End Handshake Invariant**: Test asserts that Host doorbell fires, Accelerator wakes up and computes, Host receives completion IRQ, and output data matches golden 2D convolution calculations.
- **Cache Binding Parity**: Unit tests verify that a single core connected through L1+L2 produces exact latency hits/misses, and multi-core connected through shared L2 maintains coherent data access.
- **Prior Art**: Builds on the test patterns established in `tests/baremetal_firmware_test.cpp` and `tests/unit/test_uart_timer.cpp`.

## Out of Scope

- Multi-channel PCIe root complex protocol modeling (TLP packet reordering, Gen3/Gen4 PHY negotiation).
- Full SSD Flash Translation Layer (FTL) wear-leveling and multi-plane NAND flash block management (deferred to subsequent SSD-specific milestone).
- Symmetric host OS multi-threading for cores (deterministic quantum co-simulation is used).

## Further Notes

- The Convolution Accelerator serves as the direct hardware design pattern for the future SSD Module: once the internal-CPU + Mailbox + Scratchpad pattern is verified, the accelerator can be substituted with an SSD controller and NAND Flash interface.
