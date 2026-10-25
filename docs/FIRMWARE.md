# Firmware CLI

Use `--firmware` to boot a Cortex-M ELF on the default TinyCpuSim SoC:

```sh
cmake --build build --target baremetal_firmware_elf -j2
build/tinycpusim --firmware build/isa_boot_acceptance.elf
```

Firmware mode loads ELF segments at their load addresses, attaches the SoC
peripherals and NVIC, sets VTOR to the firmware flash base (`0x00010000`), and
loads MSP and Reset_Handler from the vector table before execution. UART0
transmit text is printed after the simulation. Existing options such as
`--max-steps` still apply. Without `--firmware`, ELF loading and execution keep
the generic CLI behavior.
