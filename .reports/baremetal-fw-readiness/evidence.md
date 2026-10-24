# Evidence and exact commands

Source HEAD: `6f0f42e58686fd43c5c7ab7b0192121b02b965de`.

## Acceptance test

Command: `ctest --test-dir build -R baremetal_firmware_test --output-on-failure`

Outcome: 5/5 passed (tests 269–273): FullPeripheralSystemExecution; ClockedPeripheralsAndTimerInterruptViaInterpreter; GpioDebounceAndDmaBurstWithInterruptsViaInterpreter; VectorResetAndPrimaskPreserveInterruptedContext; CompiledCortexMBootInitializesDataAndBssAndExits.

## Public CLI probes

Command: `./build/tinyarmsim build/isa_boot_acceptance.elf`

Outcome: simulation failed with exit/R0=1 after 88 instructions. This executable path does not attach SoC peripherals, use `ElfLoadMode::Firmware`, set VTOR, or call `boot_cortex_m`; see `src/main.cpp:242-289`.

Command: `./build/tinyarmsim /tmp/task051/demo.elf`

Outcome: simulation failed after 61 instructions with `Undefined instruction 0xF04F4280 at PC 0x00000000` (CLI reported PC). `demo.elf` is the existing ELF noted in the task. This does not isolate an ISA fault from missing firmware board setup.

A preliminary command against `/tmp/task051-reliable/demo-O2.elf` returned input-open error because that path does not exist; the reliable directory contains `demo-O2.elf`, while `/tmp/task051/demo.elf` was used for the requested demo probe. A preliminary non-firmware `tests/fixtures/test_arithmetic.elf` CLI smoke run passed with exit 0; it is not counted as firmware evidence.

## Direct board-runner demo

Command: `/tmp/task051-reliable/fw-runner /tmp/task051/demo.elf 0`

Outcome: runner returned 2; actual exception PC `0x101f8`, halfwords `0xF04F 0x4280`, message `Undefined instruction 0xF04F4280 at PC 0x00000000`. Runner setup is confirmed by source: SoC peripherals attached, VTOR set to `0x10000`, Firmware load mode, and `boot_cortex_m` at `0x10000`. Therefore the demo has a real instruction gap after board initialization. The CLI-reported PC for the same encoding was 0 because its generic reporter differs; use runner’s captured state PC for actual fault location.

## Reliable natural-C evidence supplied for this continuation

Input artifact: `/tmp/task051-reliable/results.json` (12 fresh rows). Function source: `/tmp/task051-reliable/probes.c`. Harness source: `/tmp/task051-reliable/direct.cpp`. Exact runner source: `/tmp/task051-reliable/fw-runner.cpp`. The reliable artifact reports direct-call initialization after C function load, R0 input assignment after load, SP=`0x30000`, LR=`0x8001` sentinel. It avoids startup/BSS overlap with input. No prior invalid `/tmp/task051` semantic-zero results are used.

Compiler identity: `arm-none-eabi-gcc (15:13.2.rel1-2) 13.2.1 20231009`. Ordinary function-probe flags recorded by artifact: `-mcpu=cortex-m3 -mthumb -O0` or `-O2`, freestanding ELF/linker path; no `-fno-if-conversion` flags. This invocation attempted a separate reproduction by linking `startup.s` and `probes.c` directly; both attempts failed because `probes.c` has functions but no `main` (`undefined reference to main`). This failed check did not alter artifacts and is not a probe result. The validated 12-row results remain source-of-record.

Observed rows: O0 select inputs 0/1 => 9/7 pass; O2 select 0/1 => 7/7 (input 0 mismatch); negate inputs `0x80` and `0xffffff80` => 0 at O0/O2 (four mismatches); compare inputs `0xffffff80` and `0xffffff81` fault at O0 `F113 0F80`, PC `0x1003e`, and O2 `F110 0F80`, PC `0x10010` (four faults). Counts: 2 pass, 10 not passing expected behavior (4 wrong values from negate, 2 wrong select results, and 4 compare faults: two inputs at each optimization level).

Exact passing direct harness pattern in the validated source: `MemoryBus bus(64u*1024u*1024u); ArchitecturalState s; IsaInterpreter cpu(s,bus); Loader::load_elf(f,bus,s,Loader::ElfLoadMode::Firmware); s.set_pc(entry & ~1u); s.set_sp(0x30000); s.set_lr(0x8001); s.set_reg(0,input);` then step until PC reaches `0x8000` sentinel (maximum 200 steps); capture fault PC and raw halfwords on exception. The post-load `set_reg(0,input)` is essential for reliable direct-function semantics.

The board-runner source `/tmp/task051-reliable/fw-runner.cpp` documents `SoCPeripherals::attach_all`, `soc.nvic->set_vtor(0x10000)`, Firmware load mode and `Loader::boot_cortex_m`; no successful demo-board runner result was supplied in this invocation. Do not infer demo health from acceptance tests.

No product, test, fixture, or ELF files were edited. The five declared report files are the only intended outputs.
