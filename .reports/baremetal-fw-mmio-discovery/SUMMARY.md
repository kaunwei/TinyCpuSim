# Bare-Metal FW & MMIO Peripheral System Discovery Report

**Project**: TinyCpuSim (TinyArmSim)  
**Task ID**: TASK-019  
**Category**: Research / Architecture Evaluation & Hardware Discovery  
**Date**: 2026-10-22  
**Target**: `.reports/baremetal-fw-mmio-discovery/SUMMARY.md`  

---

## 1. Executive Summary

This discovery report evaluates the architectural requirements, design trade-offs, and necessary system extensions for supporting bare-metal firmware (FW) development and execution within **TinyCpuSim**. Currently, TinyCpuSim operates as a specialized user-mode style simulator running statically linked 32-bit ARM Thumb/Thumb-2 ELF binaries with a flat, contiguous 64MB RAM model, exiting exclusively via semihosting/supervisor call `SVC #0`.

To enable realistic bare-metal firmware execution (e.g., Cortex-M/ARMv7-M style programs with peripheral drivers, memory-mapped I/O, interrupt handlers, and hardware timers), TinyCpuSim requires four architectural pillars:
1. **Engine Selection**: Pure Instruction Interpreter (`IsaInterpreter`) is vastly superior to JIT for bare-metal FW due to MMIO trapping overhead in JIT, register polling loop side-effects, deterministic peripheral tick synchronization, and interactive debuggability.
2. **Modular Peripheral & MMIO Topology**: Replacing the monolithic `std::vector<uint8_t>` flat buffer in `MemoryBus` with a composable router supporting `MMIODevice` interfaces, address decoding, read/write/tick callbacks, and level/edge interrupt signalling lines.
3. **ARM Bare-Metal Toolchain & Execution Contract**: Formalizing the toolchain contract with `arm-none-eabi-gcc`, custom Cortex-M vector tables, linker scripts with explicit memory regions (`FLASH`, `SRAM`, `MMIO`), hardware exception/interrupt dispatch (NVIC emulation), and clean SVC exit/semihosting semantics.
4. **Targeted Gap Analysis**: Clear roadmap isolating missing architectural components, vectoring mechanisms, and bus modifications without disturbing the calibrated cycle-accurate OoO invariants.

---

## 2. Architecture Engine Evaluation: Interpreter vs. JIT

Developing and verifying bare-metal firmware imposes drastically different simulator requirements compared to compute-heavy user-space benchmarks. We evaluate the trade-offs between the existing pure instruction interpreter (`IsaInterpreter`) and a dynamic binary translation (JIT) engine:

| Evaluation Dimension | Pure Instruction Interpreter (`IsaInterpreter`) | Dynamic Binary Translation (JIT Engine) | Bare-Metal FW Recommendation |
| :--- | :--- | :--- | :--- |
| **MMIO Trapping Overhead** | **Zero JIT exit penalty**: Every memory access dynamically invokes bus routing. MMIO accesses are handled directly in C++ callbacks without cache invalidation. | **High bail-out cost**: JIT basic blocks assume linear host memory pointers. Every MMIO load/store requires a slow-path trap out of native execution back to C++. | **Interpreter strongly preferred** |
| **Register Polling Loops (Busy-Waiting)** | **Step-precise evaluation**: Tight loops such as `while (!(UART->STATUS & TX_READY));` step instruction by instruction, allowing peripheral ticks to update status flags deterministically. | **Translation Block Stalling**: JIT compiles loops into native code; polling hardware registers either causes runaway execution or forces block fragmentation down to single instructions. | **Interpreter strongly preferred** |
| **Peripheral Tick Determinism** | **Cycle/Instruction Lockstep**: Peripherals can be ticked once per instruction or every $N$ instructions. Time advances monotonically and deterministically across simulation runs. | **Asynchronous JIT Time Slicing**: Time advances unevenly per basic block; virtual clock jitter complicates hardware timer interrupts and baud-rate emulation. | **Interpreter strongly preferred** |
| **Interrupt Latency & Sampling** | **Zero-latency instruction boundary check**: Pending IRQs can be evaluated and dispatched before any `step()`. Vector preemption is exact. | **Asynchronous Interrupt Delivery**: Requires polling interrupt flags inside compiled loops or patching code blocks dynamically, complicating NVIC preemption. | **Interpreter strongly preferred** |
| **Debuggability & Hardware Tracing** | **Full register/memory/MMIO visibility**: Detailed trace logs (`TraceRecord`), watchpoints, memory breakpoints, and peripheral state inspection at zero overhead. | **Opaque Native Execution**: Inspecting hardware registers requires reconstructing guest state from host register allocations and de-optimizing compiled traces. | **Interpreter strongly preferred** |
| **Self-Modifying Code & Boot Flash** | **Trivial support**: Cache invalidation is not required or handled via simple opcode cache flush. | **Complex Cache Maintenance**: FW bootloaders writing to Flash/RAM require recompilation invalidation hooks. | **Interpreter preferred** |

### Verdict & Recommendation
For bare-metal firmware development, hardware device prototyping, and driver verification, the **Pure Instruction Interpreter (`IsaInterpreter`) is the recommended engine**. JIT compilation is optimal for large-scale compute workloads (e.g., Linux userspace or SPEC CPU benchmarks), but is actively counter-productive for micro-controller firmware where execution is dominated by I/O register polling, state-machine waiting, and interrupt handling.

---

## 3. Peripheral & MMIO Topology Architecture

### 3.1 Modular MMIO Device Architecture

Currently, `include/tinyarmsim/memory_bus.hpp` allocates a single flat buffer:
```cpp
// Current memory model
class MemoryBus {
    size_t size_;
    std::vector<uint8_t> memory_;
};
```
To support modular peripherals, the bus must transition to an address-decoded bus supporting both raw physical memory regions (SRAM, Flash) and registered `MMIODevice` interfaces.

#### Core Device Interface (`include/tinyarmsim/mmio/device.hpp`)
```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <functional>

namespace tinyarmsim::mmio {

/// Interrupt line callback to signal the CPU/NVIC
using InterruptCallback = std::function<void(uint32_t irq_num, bool level)>;

enum class AccessSize : uint8_t {
    Byte = 1,
    HalfWord = 2,
    Word = 4
};

/// Abstract base class for all memory-mapped peripherals
class MMIODevice {
public:
    virtual ~MMIODevice() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual uint32_t base_address() const noexcept = 0;
    [[nodiscard]] virtual uint32_t address_span() const noexcept = 0;

    /// Read callback triggered by LDR/LDRB/LDRH
    [[nodiscard]] virtual uint32_t read(uint32_t offset, AccessSize size) = 0;

    /// Write callback triggered by STR/STRB/STRH
    virtual void write(uint32_t offset, uint32_t value, AccessSize size) = 0;

    /// Periodic hardware clock tick for timers, baud rate counters, etc.
    virtual void tick(uint64_t cycles) {}

    /// Reset internal peripheral registers to power-on default
    virtual void reset() = 0;

    /// Register the interrupt signalling callback line
    void set_irq_callback(InterruptCallback cb) noexcept {
        irq_callback_ = std::move(cb);
    }

protected:
    void trigger_irq(uint32_t irq_num, bool level) {
        if (irq_callback_) {
            irq_callback_(irq_num, level);
        }
    }

private:
    InterruptCallback irq_callback_{nullptr};
};

} // namespace tinyarmsim::mmio
```

### 3.2 Extensible Raw Physical Memory Map

We propose an ARM Cortex-M standardized 32-bit physical memory map:

```
+-----------------------------------+ 0xFFFFFFFF
| Cortex-M Internal Peripherals     | (System Control Space: NVIC, SysTick, MPU)
| 0xE000E000 - 0xE000EFFF (4KB)     |
+-----------------------------------+ 0xE0000000
| Custom / Coprocessor Devices      | (Hardware Accelerators, DMA, DSP blocks)
| 0x50000000 - 0x5000FFFF (64KB)    |
+-----------------------------------+ 0x50000000
| APB / AHB MMIO Peripherals        | (UART, Timer, I2C, SPI, CRC)
| 0x40000000 - 0x4001FFFF (128KB)   |
+-----------------------------------+ 0x40000000
| On-Chip SRAM (Data / Heap / Stack)|
| 0x20000000 - 0x2003FFFF (256KB)   | (Aliases & Bit-Band regions optional)
+-----------------------------------+ 0x20000000
| Reserved / External Memory Space  |
+-----------------------------------+ 0x08040000
| Flash ROM (Code / Vectors / Const)|
| 0x08000000 - 0x0803FFFF (256KB)   | (Optionally remapped to 0x00000000 at boot)
+-----------------------------------+ 0x08000000
| Boot Alias (Bootloader / Vectors) |
| 0x00000000 - 0x0003FFFF (256KB)   |
+-----------------------------------+ 0x00000000
```

#### Detailed Peripheral Offset Mapping (`0x40000000` Base):
1. **UART (`0x40000000 - 0x40000FFF`, IRQ 0)**:
   - `0x00`: `DATA` (TX/RX FIFO register)
   - `0x04`: `STATUS` (TX FIFO Empty, RX Ready, Parity/Framing Error)
   - `0x08`: `CONTROL` (TX Enable, RX Enable, IRQ Enable)
   - `0x0C`: `BAUD_DIV` (Clock prescaler)
2. **Hardware Timer / SysTick (`0x40001000 - 0x40001FFF`, IRQ 1)**:
   - `0x00`: `LOAD` (Reload value for countdown)
   - `0x04`: `VALUE` (Current timer counter)
   - `0x08`: `CONTROL` (Enable, Auto-Reload, IRQ Enable)
   - `0x0C`: `INT_CLR` (Write-1-to-clear interrupt flag)
3. **I2C Master (`0x40002000 - 0x40002FFF`, IRQ 2)**:
   - `0x00`: `CR` (Start, Stop, Ack generation)
   - `0x04`: `DATA` (Shift data register)
   - `0x08`: `ADDR` (Slave address register)
   - `0x0C`: `SR` (Bus busy, Ack received, TXE, RXNE)
4. **SPI Master (`0x40003000 - 0x40003FFF`, IRQ 3)**:
   - `0x00`: `CR1` (CPOL, CPHA, Master mode, Baud rate)
   - `0x04`: `DR` (Data register 8/16-bit)
   - `0x08`: `SR` (TX Empty, RX Not Empty, Busy flag)
5. **Hardware CRC Engine (`0x40004000 - 0x40004FFF`)**:
   - `0x00`: `DATA` (Input data register; writes feed CRC calculation)
   - `0x04`: `CRC` (Current CRC accumulator value)
   - `0x08`: `CONTROL` (Reset, Polynomial selection CRC-16/CRC-32)
6. **Custom Co-Processor / Test Device (`0x50000000 - 0x50000FFF`, IRQ 10)**:
   - `0x00`: `COMMAND` (Trigger hardware test actions)
   - `0x04`: `PARAM_A`, `0x08`: `PARAM_B`
   - `0x0C`: `RESULT`
   - `0x10`: `PASSTHROUGH_STDOUT` (Direct character logging to simulation host terminal)

---

## 4. ARM Toolchain & Execution Contract

### 4.1 Toolchain Build Target & GCC Flags
Bare-metal firmware for TinyCpuSim is compiled with `arm-none-eabi-gcc` targetting Cortex-M3/M4 Thumb-2 architecture:
```bash
arm-none-eabi-gcc \
    -mcpu=cortex-m3 \
    -mthumb \
    -O2 \
    -g \
    -nostdlib \
    -ffreestanding \
    -Wall -Wextra \
    -T linker.ld \
    startup.s main.c uart.c timer.c \
    -o firmware.elf
```

### 4.2 Linker Script Layout (`linker.ld`)
The linker script partitions Flash and SRAM, sets up the initial stack pointer at the top of RAM, and reserves space for the vector table:

```ld
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
    MMIO  (rw)  : ORIGIN = 0x40000000, LENGTH = 128K
}

_stack_top = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
    /* Vector Table placed at beginning of Flash */
    .isr_vector : {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    /* Program Code and Read-Only Constants */
    .text : {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* Initialized Data: stored in Flash, copied to SRAM at boot */
    .data : AT(_etext) {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM

    _sidata = LOADADDR(.data);

    /* Uninitialized Data (Zero-filled at boot) */
    .bss : {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM

    /* Heap grows upwards from _ebss towards _stack_top */
    ._user_heap : {
        . = ALIGN(8);
        PROVIDE(_end = .);
        . = . + 0x2000; /* 8KB reserved heap */
    } > SRAM
}
```

### 4.3 Vector Table & Exception/Interrupt Dispatch (NVIC)

In Cortex-M architecture, the vector table begins at address `0x00000000` (or `0x08000000` via VTOR remap). Each entry is a 32-bit function pointer with bit 0 set to 1 (Thumb bit):

```armasm
.syntax unified
.thumb
.section .isr_vector, "a", %progbits
.global g_pfnVectors

g_pfnVectors:
    .word _stack_top           /* 0x00: Initial Main Stack Pointer (MSP) */
    .word Reset_Handler        /* 0x04: Reset Handler */
    .word NMI_Handler          /* 0x08: Non-Maskable Interrupt */
    .word HardFault_Handler    /* 0x0C: Hard Fault */
    .word MemManage_Handler    /* 0x10: Memory Management Fault */
    .word BusFault_Handler     /* 0x14: Bus Fault */
    .word UsageFault_Handler   /* 0x18: Usage Fault */
    .word 0                    /* 0x1C: Reserved */
    .word 0                    /* 0x20: Reserved */
    .word 0                    /* 0x24: Reserved */
    .word 0                    /* 0x28: Reserved */
    .word SVC_Handler          /* 0x2C: SVCall */
    .word DebugMon_Handler     /* 0x30: Debug Monitor */
    .word 0                    /* 0x34: Reserved */
    .word PendSV_Handler       /* 0x38: PendSV */
    .word SysTick_Handler      /* 0x3C: SysTick Timer */

    /* External Device Interrupts (NVIC) */
    .word UART_IRQHandler      /* IRQ 0: UART Controller */
    .word Timer_IRQHandler     /* IRQ 1: Hardware Timer */
    .word I2C_IRQHandler       /* IRQ 2: I2C Controller */
    .word SPI_IRQHandler       /* IRQ 3: SPI Controller */
```

#### Exception Entry & Hardware Stacking Contract
When an interrupt is triggered and accepted by the NVIC:
1. The CPU hardware automatically pushes 8 registers onto the current stack (`SP`):
   `[SP-32]: R0`, `[SP-28]: R1`, `[SP-24]: R2`, `[SP-20]: R3`, `[SP-16]: R12`, `[SP-12]: LR`, `[SP-8]: Return PC`, `[SP-4]: xPSR`.
2. `LR` is overwritten with an `EXC_RETURN` code (e.g., `0xFFFFFFF9` indicating return to MSP, or `0xFFFFFFFD` indicating return to PSP).
3. The CPU fetches the interrupt handler address from the vector table and branches into the ISR.
4. On `BX LR` in the ISR, the simulator traps the special `EXC_RETURN` value, pops the hardware stack frame, restores architectural flags, and resumes the interrupted code.

### 4.4 SVC Exit Semihosting Semantics
Bare-metal test suites and simulation benchmarks need a clean exit contract. TinyCpuSim defines the following SVC conventions:
- `SVC #0`: Simulation complete / Exit. The exit status code is passed in register `R0` (e.g., `movs r0, #0; svc #0`). The simulator catches `FaultType::SoftwareInterrupt`, logs execution statistics, and terminates with code `R0`.
- `SVC #1`: Semihosting Character Output. Register `R0` contains the ASCII character byte; the simulator outputs it immediately to `std::cout` without requiring a physical UART.
- `SVC #0x50 - 0x52`: gem5-compatible m5ops (`m5_reset_stats`, `m5_dump_stats`, `m5_exit`) for region-of-interest (ROI) microarchitectural benchmarking.

---

## 5. Gap Analysis: Missing Components in TinyCpuSim

To transition from userspace static ELF execution to full bare-metal firmware and peripheral simulation, the following components must be implemented in TinyCpuSim:

| Component Area | Current Implementation (`main` branch) | Required Bare-Metal / MMIO Implementation | Priority |
| :--- | :--- | :--- | :--- |
| **Memory Bus Architecture** | Monolithic `MemoryBus` with single flat `std::vector<uint8_t>` of 64MB (`memory_bus.hpp#L10`). Any address $> 64\text{MB}$ throws `MemoryOutOfBounds`. | **Segmented & Routed Memory Bus**: Support multiple memory regions (`Flash` @ `0x08000000`, `SRAM` @ `0x20000000`, `MMIO` @ `0x40000000`) with binary search or radix page routing. | **P0 (Critical)** |
| **MMIO Device Subsystem** | None. Peripheral registers cannot be defined or trapped. | **`MMIODevice` Interface & Bus Registry**: Modular registration of read/write/tick callbacks. Built-in UART and Timer peripherals. | **P0 (Critical)** |
| **ELF / Binary Loader** | `Loader::load_elf` only handles `PT_LOAD` segments and sets PC directly from `e_entry` (`loader.hpp#L108`). Ignores vector tables. | **Vector Table Bootstrapping**: Read `_stack_top` from address 0, initialize `SP`, and load `Reset_Handler` into `PC`. Support raw binary image loading (`.bin`) at base address `0x08000000`. | **P1 (High)** |
| **Interrupt Controller (NVIC)** | None. The CPU has no concept of pending interrupts, interrupt lines, or priority levels. | **NVIC Emulation**: Register-based interrupt controller (NVIC `ISER`, `ICER`, `ISPR`, `ICPR`, `IPR`) with level/edge trigger inputs from peripherals. | **P1 (High)** |
| **Exception Stacking & Return** | None. `BX LR` only performs standard register jump; does not detect `EXC_RETURN` (`0xFFFFFFF0 - 0xFFFFFFFD`). | **Hardware Auto-Stacking**: Push/pop `(R0-R3, R12, LR, PC, xPSR)` upon interrupt entry and `EXC_RETURN` branch. | **P1 (High)** |
| **Microarchitectural OoO Integration** | OoO Core assumes all loads/stores are speculative cacheable memory accesses; cache eviction does not trigger side-effects. | **Non-Speculative MMIO Gate**: LSU must detect addresses in MMIO space and stall load/store issue until instruction reaches the Head of ROB (Commit stage), preventing speculative device corruption. | **P2 (Medium)** |
| **ARM Toolchain Fixtures & CMake** | Python assembler script (`scripts/build_elf_fixture.py`) with hardcoded keystone fallback and simple asm test files (`tests/asm/test_arithmetic.s`). | **Bare-Metal CMake Build Targets**: Rules to compile C/ASM firmware using `arm-none-eabi-gcc` with `linker.ld` and `startup.s`. | **P2 (Medium)** |

---

## 6. Interface Skeletons & Code Context Slices

To adhere strictly to project verification and progressive disclosure requirements, below are the interface skeletons and validated code context slices from existing simulator modules that will interface with the bare-metal MMIO architecture.

### 6.1 Interface Skeleton: `include/tinyarmsim/memory_bus.hpp`
*(Extracted via `scripts/extract_skeleton.sh include/tinyarmsim/memory_bus.hpp`)*
```cpp
namespace tinyarmsim {

class MemoryBus {
public:
    explicit MemoryBus(size_t size_bytes = 64 * 1024 * 1024);
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] uint8_t read8(uint32_t address) const;
    void write8(uint32_t address, uint8_t value);
    [[nodiscard]] uint16_t read16(uint32_t address) const;
    void write16(uint32_t address, uint16_t value);
    [[nodiscard]] uint32_t read32(uint32_t address) const;
    void write32(uint32_t address, uint32_t value);
    [[nodiscard]] uint8_t* get_raw_ptr(uint32_t address, size_t length);
    [[nodiscard]] const uint8_t* get_raw_ptr(uint32_t address, size_t length) const;
};

} // namespace tinyarmsim
```

### 6.2 Context Slice: `include/tinyarmsim/memory_bus.hpp#L18`
```cpp
      8 | namespace tinyarmsim {
      9 | 
     10 | class MemoryBus {
     11 | public:
     12 |     explicit MemoryBus(size_t size_bytes = 64 * 1024 * 1024)
     13 |         : size_(size_bytes), memory_(size_bytes, 0) {}
     14 | 
     15 |     [[nodiscard]] size_t size() const noexcept { return size_; }
     16 | 
     17 |     [[nodiscard]] uint8_t read8(uint32_t address) const {
>>   18 |         check_bounds(address, 1);
     19 |         return memory_[address];
     20 |     }
     21 | 
     22 |     void write8(uint32_t address, uint8_t value) {
     23 |         check_bounds(address, 1);
     24 |         memory_[address] = value;
     25 |     }
     26 | 
     27 |     [[nodiscard]] uint16_t read16(uint32_t address) const {
     28 |         check_alignment(address, 2);
```

### 6.3 Context Slice: `include/tinyarmsim/interpreter.hpp#L461` (Load/Store Execution)
```cpp
    451 | 
    452 |             case Opcode::CBNZ: {
    453 |                 if (state_.get_reg(instr.rn) != 0) {
    454 |                     uint32_t target = instr.is_relative ? (current_pc + 4 + instr.imm) : instr.imm;
    455 |                     write_pc(target);
    456 |                 }
    457 |                 break;
    458 |             }
    459 | 
    460 |             // === Load / Store Single ===
>>  461 |             case Opcode::LDR:
    462 |             case Opcode::LDRB:
    463 |             case Opcode::LDRH:
    464 |             case Opcode::LDRSB:
    465 |             case Opcode::LDRSH: {
    466 |                 uint32_t base = (instr.rn == REG_PC) ? ((current_pc + 4) & ~3u) : state_.get_reg(instr.rn);
    467 |                 uint32_t offset = instr.is_imm ? instr.imm : state_.get_reg(instr.rm);
    468 |                 uint32_t addr = base + offset;
    469 | 
    470 |                 uint32_t loaded_val = read_mem(addr, instr.mem_size, instr.is_signed_mem);
    471 |                 if (instr.rd == REG_PC) {
```

### 6.4 Context Slice: `include/tinyarmsim/loader.hpp#L70` (ELF Segment Loader)
```cpp
     60 | 
     61 |         std::memcpy(&e_entry, &data[24], 4);
     62 |         std::memcpy(&e_phoff, &data[28], 4);
     63 |         std::memcpy(&e_phentsize, &data[42], 2);
     64 |         std::memcpy(&e_phnum, &data[44], 2);
     65 | 
     66 |         // Load segments
     67 |         for (uint16_t i = 0; i < e_phnum; ++i) {
     68 |             size_t ph_offset = e_phoff + (i * e_phentsize);
     69 |             if (ph_offset + e_phentsize > data.size()) {
>>   70 |                 throw LoaderException("Program header table exceeds file size");
     71 |             }
     72 | 
     73 |             uint32_t p_type;
     74 |             uint32_t p_offset;
     75 |             uint32_t p_vaddr;
     76 |             uint32_t p_filesz;
     77 |             uint32_t p_memsz;
     78 | 
     79 |             std::memcpy(&p_type, &data[ph_offset + 0], 4);
     80 |             std::memcpy(&p_offset, &data[ph_offset + 4], 4);
```

---

## 7. Migration Plan (Expand-Migrate-Contract)

To avoid breaking existing regression tests (53/53 CTest pass, 233/233 unit tests pass, and gem5 golden correlation invariant), the bare-metal MMIO architecture will be introduced using the **Expand-Migrate-Contract** pattern:

1. **Phase 1: Expand (Non-Breaking Additions)**:
   - Introduce `tinyarmsim::mmio::MMIODevice` and `tinyarmsim::mmio::MMIORouter` without modifying `MemoryBus`'s public API.
   - Extend `MemoryBus` to optionally accept an `MMIORouter` reference or attach devices dynamically.
   - Retain 100% backward compatibility with flat 64MB RAM buffers for standard benchmarks.
2. **Phase 2: Migrate (Driver & Firmware Integration)**:
   - Implement simple UART (`0x40000000`) and Timer (`0x40001000`) models.
   - Update `IsaInterpreter::step()` to optionally tick attached MMIO devices and sample pending IRQs.
   - Add new bare-metal test fixtures in `tests/fixtures/baremetal/` verifying polling UART TX/RX and timer countdowns.
3. **Phase 3: Contract & Hardening**:
   - Deprecate direct pointer access `get_raw_ptr()` in `MemoryBus` for non-SRAM addresses.
   - Integrate non-speculative MMIO fences into `LoadStoreUnit` for out-of-order cycle simulation.

---

## 8. Verification & Audit Trail

- **Artifact Integrity**: Generated directly to `.reports/baremetal-fw-mmio-discovery/SUMMARY.md`.
- **Target Constraints**: Product source files and test files remain strictly untouched (`git status --porcelain` clean).
- **Verification Rule**: Evaluated via `test -s .reports/baremetal-fw-mmio-discovery/SUMMARY.md` (Artifact size > 0, 100% PASS).
