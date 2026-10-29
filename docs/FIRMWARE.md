# Firmware CLI

Use `--firmware` to boot a Cortex-M ELF on the default TinyCpuSim SoC:

```sh
cmake --build build --target baremetal_firmware_elf -j2
build/tinycpusim --firmware build/isa_boot_acceptance.elf
```

Firmware mode loads ELF segments at their load addresses, attaches the SoC
peripherals and NVIC, sets VTOR to the firmware flash base (`0x00010000`), and
loads MSP and Reset_Handler from the vector table before execution. Existing
options such as `--max-steps` still apply. Without `--firmware`, ELF loading
and execution keep the generic CLI behavior.

## Live UART Output & Control Channel

When firmware boots with UART0 enabled, transmitted bytes are streamed live to
standard output (`STDOUT_FILENO`) via `FirmwareUartOutput` rather than buffered
until simulation completion. When a consumer is active, duplicate replaying of
already streamed UART bytes at shutdown is suppressed.

## Foreground Signal Handling & Exit Status

A foreground `FirmwareSignalGuard` manages SIGINT and SIGTERM during execution:
- Interrupt signals are latched asynchronously and abort execution cleanly.
- When interrupted by a signal, TinyCpuSim exits with status `128 + signal`
  (e.g., 130 for SIGINT, 143 for SIGTERM).
- Signal masks, actions, and descriptor flags are restored reliably via RAII.
- On shutdown, pending UART bytes are drained with a bounded flush. Errors
  such as closed pipes (`EPIPE`) or buffer overflow are handled without crashing
  from SIGPIPE and reported via stderr and non-zero exit status.
