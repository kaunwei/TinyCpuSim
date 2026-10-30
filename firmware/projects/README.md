# TinyCpuSim Firmware Projects

Each subdirectory under `firmware/projects/` is an independent baremetal firmware project.

## Directory Structure & Conventions

```text
firmware/projects/<project_name>/
├── main.c           # Project application logic
├── Makefile         # Build rules (reuses ../../bsp/startup.s & ../../bsp/linker.ld)
├── banner.txt       # (Optional) Custom string printed when run.sh executes this project
└── .gitignore       # Ignores *.elf and *.o
```

## Shared BSP Assets

All projects share common Cortex-M3 initialization and memory layout from `firmware/bsp/`:
- **Startup file**: `../../bsp/startup.s` (vector table, stack init, bss zeroing, `main` call, clean exit via `svc #0`)
- **Linker script**: `../../bsp/linker.ld` (FLASH & RAM memory regions)
- **Peripherals**: `../../bsp/uart.c`, `../../bsp/timer.c`, etc.

The project directory only needs to contain its own `.c` and `.h` files!

## Customizing the `run.sh` Banner

In your project directory, create a `banner.txt` file. When you run `./run.sh fw <project_name>`, the launcher will automatically read and display your custom banner before launching the simulator.

## Running Projects

```bash
# Interactive selection menu
./run.sh fw

# Directly run a specific project
./run.sh fw uart_echo

# Run with PTY connection info for external serial tools (picocom, screen)
./run.sh fw uart_echo --pty

# Build all projects
./run.sh fw build
```
