<!-- REPORT_META: Task=TASK-051 | Commit=6f0f42e58686fd43c5c7ab7b0192121b02b965de -->
# Verified observations and exact commands

Working directory is the human repository root. Natural-function and board-runner binaries were compiled from current source headers at the metadata commit. Product sources were not modified.

## Positive bounded firmware acceptance

`.worker-tree/build/baremetal_firmware_test` was run directly: **5/5 PASS**, cases listed in probe_results.json. The separate proper CTest command `/usr/bin/ctest --test-dir .worker-tree/build -R CompiledCortexMBootInitializesDataAndBssAndExits --no-tests=error --output-on-failure` ran **1/1 PASS**. These are distinct measured commands, not an executable-name filter assumption.

`/tmp/task051-reliable/fw-runner .worker-tree/build/isa_boot_acceptance.elf 0`: rc0, finalPC0x100AC. The fixture uses O1/no-if-conversion and proves its own bounded contract.

## Fresh normal-O2 unchanged BSP/demo (primary)

```bash
/usr/bin/g++ -std=c++20 -I .worker-tree/include /tmp/task051-reliable/direct.cpp -o /tmp/task051-reliable/direct
/usr/bin/g++ -std=c++20 -I .worker-tree/include /tmp/task051-reliable/fw-runner.cpp -o /tmp/task051-reliable/fw-runner
/usr/bin/arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -ffreestanding -fno-builtin -nostdlib -O0 -Wl,-Ttext=0x10000,-e,probe_select /tmp/task051-reliable/probes.c -o /tmp/task051-reliable/O0.elf
/usr/bin/arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -ffreestanding -fno-builtin -nostdlib -O2 -Wl,-Ttext=0x10000,-e,probe_select /tmp/task051-reliable/probes.c -o /tmp/task051-reliable/O2.elf
/usr/bin/arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -O2 -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -I .worker-tree/firmware/bsp -T .worker-tree/firmware/bsp/linker.ld .worker-tree/firmware/bsp/startup.s .worker-tree/firmware/bsp/*.c .worker-tree/firmware/apps/demo_all_peripherals.c -o /tmp/task051-reliable/demo-O2.elf
/tmp/task051-reliable/fw-runner /tmp/task051-reliable/demo-O2.elf 0
/tmp/task051-reliable/fw-runner .worker-tree/build/isa_boot_acceptance.elf 0
```

The freshly compiled demo first fails at **actualPC0x10204, F04F4380, MOV.W r3,#0x40000000 in uart_init**. Board composition and reset have occurred; the first failure precedes UART register access. Later peripherals are not assessed by this halted run.

Secondary older ELF: `/tmp/task051-reliable/fw-runner /tmp/task051/demo.elf 0` fails at actualPC0x101F8 F04F4280 MOV.W r2,#0x40000000. The older file is not substituted for fresh compilation provenance.

## Exact public CLI commands

`.worker-tree/build/tinyarmsim --elf .worker-tree/build/isa_boot_acceptance.elf --max-steps 50000`: exit/R0=1,88instructions, no explicit CPU fault.

`.worker-tree/build/tinyarmsim --elf /tmp/task051/demo.elf --max-steps 50000`: exit1 after61instructions, UndefinedInstruction F04F4280, error-messagePC0. Source src/main.cpp#L266 uses default ELF load; no SoC attach or vector reset appears in main. Board-runner captured architectural PC, rather than generic exception messagePC, establishes the actual fault location.

## Natural C semantics

Full sources/commands in probe_sources.md; all12 raw rows in probe_results.json. Inputs are R0 assigned **after** load, SP0x30000, LR0x8001, return sentinel0x8000. No startup executes for these direct functions, and no BSS symbols carry inputs/results. The earlier TASK051 harness with startup-erased input or reordered BSS is not used.

**3 PASS,9 FAIL**: O0select0/1 ->9/7 (2pass); O2select0/1 ->7/7 (1wrong,1pass). NEGS(+128/-128) returns0 at O0/O2 (4wrong; expected-128/+128). Negative-literal compare(-128/-127) faults at CMN.W O0PC0x1003E F1130F80 / O2PC0x10010 F1100F80 (4faults).

O2select disassembly: 2800 CMP r0,#0; BF0C ITE EQ; 2009 MOVEQ r0,#9; 2007 MOVNE r0,#7;4770 BX LR. Both assignments execute because IT is treated as NOP. Negation words are O0 425B and O2 4240. Compare reaches CMN.W before its later ITE, so its measured failure must not be attributed to IT. Tested signed inputs+/-128 are ordinary defined arithmetic inputs.

No gem5 calibration was reopened; no claim of broad ISA, BSP or RTOS readiness follows from the positive curated fixture.
