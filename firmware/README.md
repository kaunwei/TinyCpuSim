# Baremetal Firmware & Peripheral Subsystem

This directory contains the Cortex-M3 (ARMv7-M) baremetal firmware environment for TinyCpuSim, including the Board Support Package (BSP), built-in acceptance test applications, and user firmware projects.

---

## Table of Contents

- [Overview & Architecture](#overview--architecture)
- [Memory Map & MMIO Peripherals](#memory-map--mmio-peripherals)
- [Shared Board Support Package (BSP)](#shared-board-support-package-bsp)
- [Firmware Projects Directory Structure](#firmware-projects-directory-structure)
- [How to Create a New Firmware Project](#how-to-create-a-new-firmware-project)
- [Customizing the Startup Banner (`banner.txt`)](#customizing-the-startup-banner-bannertxt)
- [Running Firmware via `run.sh fw`](#running-firmware-via-runsh-fw)
  - [Interactive Project Selection Menu](#1-interactive-project-selection-menu)
  - [Direct Project Execution](#2-direct-project-execution)
  - [Interactive Terminal vs PTY Mode](#3-interactive-terminal-vs-pty-mode)
  - [Listing Available Projects](#4-listing-available-projects)
  - [Batch Building Firmware](#5-batch-building-firmware)
- [Built-in Applications](#built-in-applications)

---

## Overview & Architecture

TinyCpuSim provides an authentic embedded hardware emulation environment for ARM Cortex-M3 Thumb-2 code. Firmware binaries run directly on TinyCpuSim's ISA execution engine with complete peripheral memory-mapped I/O (MMIO), interrupt controller (NVIC), system tick, hardware timers, and bidirectional UART communication.

---

## Memory Map & MMIO Peripherals

The memory layout matches standard Cortex-M microcontroller architectures:

| Region | Address Range | Size | Description |
| :--- | :--- | :--- | :--- |
| **FLASH** | `0x00010000 - 0x0004FFFF` | 256 KB | Code (`.text`), Vector table, Constants (`.rodata`) |
| **RAM** | `0x00020000 - 0x0002FFFF` | 64 KB | Initialized data (`.data`), Zero data (`.bss`), Heap |
| **Stack** | `0x00030000` (Downwards) | - | Initial Main Stack Pointer (`_stack_top`) |
| **UART0** | `0x40000000 - 0x40000FFF` | 4 KB | 16550-style FIFO UART (DATA, STATUS, BAUD, CLKDIV) |
| **TIMER0** | `0x40001000 - 0x40001FFF` | 4 KB | 32-bit Periodic Countdown Timer with Interrupts |
| **I2C0** | `0x40002000 - 0x40002FFF` | 4 KB | Inter-Integrated Circuit Bus Controller |
| **SPI0** | `0x40003000 - 0x40003FFF` | 4 KB | Serial Peripheral Interface Controller |
| **CRC0** | `0x40004000 - 0x40004FFF` | 4 KB | CRC-32 Acceleration Engine |
| **GPIO0** | `0x40005000 - 0x40005FFF` | 4 KB | General Purpose I/O with edge/level interrupts |
| **DMA0** | `0x40006000 - 0x40006FFF` | 4 KB | Direct Memory Access Controller |
| **NVIC** | `0xE000E000 - 0xE000EFFF` | 4 KB | Nested Vectored Interrupt Controller |

---

## Shared Board Support Package (BSP)

All projects share common startup, vector table, and peripheral drivers under [`firmware/bsp/`](bsp/):

- **[`linker.ld`](bsp/linker.ld)**: Standard Cortex-M3 linker script. Defines FLASH/RAM boundaries, section alignment, and stack pointer.
- **[`startup.s`](bsp/startup.s)**:
  - Vector table definition (`_stack_top`, `Reset_Handler`, fault handlers, external IRQs).
  - Copies `.data` from FLASH to RAM.
  - Zeroes `.bss`.
  - Calls `main()`.
  - Upon return from `main()`, executes `svc #0` to cleanly signal simulation termination with the return value in `r0`.
- **[`platform.h`](bsp/platform.h)**: Base register addresses, struct mappings, and status bit definitions.
- **[`uart.h`](bsp/uart.h) / [`uart.c`](bsp/uart.c)**: `uart_init()`, `uart_putc()`, `uart_puts()`, `uart_has_rx()`, `uart_getc_nonblocking()`.
- **[`timer.h`](bsp/timer.h) / [`timer.c`](bsp/timer.c)**: Periodic countdown timer configuration and interrupt clearing.
- **[`scheduler.h`](bsp/scheduler.h) / [`scheduler.c`](bsp/scheduler.c)**: Cooperative & preemptive task scheduler.
- **[`shell.h`](bsp/shell.h) / [`shell.c`](bsp/shell.c)**: Command-line parser with history and built-in diagnostic commands.

---

## Firmware Projects Directory Structure

User firmware projects are located in [`firmware/projects/`](projects/):

```text
firmware/
├── bsp/                             # Shared Board Support Package
│   ├── linker.ld                    # Shared linker script
│   ├── startup.s                    # Shared vector table & startup
│   ├── platform.h                   # Peripheral memory map
│   ├── uart.c / uart.h              # UART driver
│   ├── timer.c / timer.h            # Timer driver
│   ├── scheduler.c / scheduler.h    # Task scheduler
│   └── shell.c / shell.h            # Interactive shell
├── projects/                        # User firmware projects
│   ├── README.md                    # Project development documentation
│   └── uart_echo/                   # 3-Round Interactive UART Echo project
│       ├── main.c                   # Project application code
│       ├── Makefile                 # Build rules (reuses BSP)
│       ├── banner.txt               # Custom message displayed by run.sh
│       └── .gitignore               # Ignores *.elf and *.o
└── apps/                            # Built-in acceptance test applications
```

---

## How to Create a New Firmware Project

Creating a new baremetal project requires **only application code and a Makefile**. You do **not** need to copy or duplicate `startup.s` or `linker.ld`.

### Step 1: Create project directory
```bash
mkdir -p firmware/projects/my_project
cd firmware/projects/my_project
```

### Step 2: Write application logic (`main.c`)
```c
#include "../../bsp/platform.h"
#include "../../bsp/uart.h"

int main(void) {
    uart_init(1U);
    uart_puts("Hello from my_project on TinyCpuSim!\r\n");

    // Do work here...

    return 0; // Clean exit via svc #0
}
```

### Step 3: Write `Makefile`
```makefile
CC = arm-none-eabi-gcc
OPT ?= -O1
CFLAGS ?= -mcpu=cortex-m3 -mthumb -ffreestanding -nostdlib $(OPT) -g -Wall -Wextra -I../../bsp
LDFLAGS ?= -T ../../bsp/linker.ld -Wl,--gc-sections

BSP_SRCS = ../../bsp/startup.s ../../bsp/uart.c
APP_SRCS = main.c
SRCS = $(BSP_SRCS) $(APP_SRCS)
TARGET = my_project.elf

all: $(TARGET)

$(TARGET): $(SRCS) ../../bsp/linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@

clean:
	rm -f $(TARGET)
```

### Step 4: Add `banner.txt` (Optional)
Create `banner.txt` to define the description string displayed when `run.sh` launches your project:
```text
============================================================
  Project: My Custom Project
  Description: Demonstration of custom firmware features.
============================================================
```

---

## Customizing the Startup Banner (`banner.txt`)

When running `./run.sh fw <project_name>`, the launcher checks for `firmware/projects/<project_name>/banner.txt`. If present, its contents are displayed in the terminal before starting the simulation.

---

## Running Firmware via `run.sh fw`

The top-level launcher provides flexible execution options:

### 1. Interactive Project Selection Menu
Run zero-args to open the interactive selection menu:
```bash
./run.sh fw
```
```text
============================================================
        TinyCpuSim Firmware Project Selector                
============================================================
Available Firmware Projects:
  [1] uart_echo            (Interactive 3-round UART echo demo)
  [2] scheduler_acceptance (Preemptive task scheduler)
  [3] shell_acceptance     (Interactive firmware command shell)
  [4] reference_app        (Reference app with timer & peripherals)
============================================================
Select project [1-4, default: 1]: 
```

### 2. Direct Project Execution
Specify the project name directly on the command line:
```bash
./run.sh fw uart_echo
```
*Note: If the `.elf` binary has not been built yet, `run.sh` automatically compiles it for you before starting the simulator.*

### 3. Interactive Terminal vs PTY Mode

- **Direct Interactive Terminal Mode (Default)**:
  ```bash
  ./run.sh fw uart_echo
  ```
  Launches with an attached bidirectional bridge. You can type directly in the current terminal window. Characters are sent to guest UART RX in real time, and guest UART TX is streamed directly to stdout.

- **External Serial Terminal via PTY (`--pty`)**:
  ```bash
  ./run.sh fw uart_echo --pty
  ```
  Launches a virtual PTY device and displays the exact connection commands for external serial terminal programs:
  ```text
  ============================================================
    Firmware UART Terminal Ready
    PTY Device: /dev/pts/8
    To connect from another terminal:
      picocom -b 115200 /dev/pts/8
      screen /dev/pts/8
  ============================================================
  ```

### 4. Listing Available Projects
```bash
./run.sh fw list
```

### 5. Batch Building Firmware
```bash
# Build all firmware projects and acceptance ELFs
./run.sh fw build

# Build a specific project only
./run.sh fw build uart_echo
```

---

## Built-in Applications

In addition to user projects in `projects/`, TinyCpuSim includes built-in verification applications in [`firmware/apps/`](apps/):

1. **`uart_echo`** ([`projects/uart_echo/main.c`](projects/uart_echo/main.c)):
   - Interactive 3-round UART echo program.
   - Echoes each typed character back to the terminal.
   - Upon pressing Enter, echoes the full line.
   - Completes 3 rounds and terminates cleanly.
2. **`scheduler_acceptance`** ([`apps/scheduler_acceptance.c`](apps/scheduler_acceptance.c)):
   - Preemptive priority task scheduler with round-robin time slicing.
   - Validates context switching, stack frame preservation, and SysTick interrupts.
3. **`shell_acceptance`** ([`apps/shell_acceptance.c`](apps/shell_acceptance.c)):
   - Interactive embedded command shell (`ping`, `help`, `status`, `compute`).
4. **`reference_app`** ([`apps/reference_app.c`](apps/reference_app.c)):
   - Full integrated platform demonstrating concurrent UART, Timer, and background worker threads.
