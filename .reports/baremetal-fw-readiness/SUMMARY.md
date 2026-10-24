<!-- REPORT_META: Task=TASK-051 | Commit=6f0f42e58686fd43c5c7ab7b0192121b02b965de -->
# Bare-metal firmware readiness

## Readiness matrix

| Probe | Outcome | Boundary |
|---|---|---|
| Existing firmware acceptance | 5/5 CTest pass | MMIO, timed devices/IRQ, GPIO/DMA, vector reset/context, compiled Cortex-M data/BSS/SVC. |
| Public CLI, compiled acceptance ELF | Fail: R0/exit 1 after 88 instructions | CLI defaults to generic ELF load then interpreter run; lacks SoC attach and Cortex-M reset setup. Not an isolated ISA finding. |
| Public CLI, `/tmp/task051/demo.elf` | Fail after 61 instructions: undefined `0xF04F4280` (CLI-reported PC `0`) | Same setup limitation; cannot attribute solely to ISA. |
| Natural C probes | 12 rows; 2 expected results, 10 mismatches/faults | `/tmp/task051-reliable/results.json`; ordinary GCC `-O0/-O2`, direct function harness, input assigned after load, SP=`0x30000`, LR=`0x8001`. |
| Full peripheral demo via board runner | Fail: undefined `0xF04F4280` at actual PC `0x101f8` | `/tmp/task051-reliable/fw-runner /tmp/task051/demo.elf 0`; board attaches SoC, sets VTOR, firmware-loads and boots. Isolated instruction limitation remains after setup. |

At O0, select returns 9/7 for inputs 0/1. At O2, select returns 7/7 (input 0 expected 9; BF0C ITE EQ). Negation returns 0 for `0x80` and `0xffffff80` at both O0/O2 (425B/4240 NEGS families). Compare against literal `-128` faults on `F113 0F80` (O0 PC `0x1003e`) and `F110 0F80` (O2 PC `0x10010`), CMN.W encodings. These examples are not a compatibility survey. C signed `-INT_MIN` is undefined; do not generalize these observations as portable C behavior. The fixture's restricted `-O1 -fno-if-conversion -fno-if-conversion2` flags are not ordinary compiler compatibility evidence.

## Topology and coupling

Generic CLI caller: `src/main.cpp:242-289` constructs flat `MemoryBus`, state and interpreter, calls default `Loader::load_elf`, then `run`. Firmware loader API: `include/tinyarmsim/loader.hpp:30-121`; Firmware mode loads PT_LOAD at p_paddr and conditionally leaves VMA zero-fill to startup, while `boot_cortex_m` reads MSP/PC from VTOR. Board composition: `include/tinyarmsim/mmio/soc_platform.hpp:19-64` registers UART/timer/I2C/SPI/CRC/GPIO/DMA/custom/NVIC and attaches NVIC. Bus ownership/tick/reset: `include/tinyarmsim/memory_bus.hpp:14-105`; register/flag state: `include/tinyarmsim/state.hpp:22-73`. BSP startup copies .data, clears .bss, calls main and SVC (`firmware/bsp/startup.s:31-63`); linker FLASH/RAM map is `firmware/bsp/linker.ld:3-41`.

Compiled firmware acceptance independently composes board attach and verifies ELF symbols/data/BSS at `tests/baremetal_firmware_test.cpp:366-470`, then checks boot/run results later in that test. CLI failures are separate from the direct board-runner result: the latter has SoC/reset setup and still faults at a concrete instruction. CLI setup is incomplete; demo also exposes an ISA gap on the board path.

## Expand–Migrate–Contract matrix

| Phase | Seam and gate |
|---|---|
| Expand | Add opt-in board firmware entry path around SoC attach, Firmware load and vector reset; preserve generic CLI. Gate: CLI acceptance passes. |
| Migrate | Route compiled acceptance through that public seam, retaining focused unit tests. Gate each migration green; generic ELF behavior remains covered. |
| Contract | Remove duplicate test setup only after public path covers data/BSS, SVC and IRQ/reset. Gate: all five acceptance cases remain green and no old composition callers remain. |

No implementation proposed. Metadata source HEAD is unchanged. `scripts/extract_skeleton.sh` is absent from this worker checkout; `03-micro.md` contains bounded declaration/call skeletons and 10-line slices instead.

## Seam digest

Seam: `Loader::load_elf` / `Loader::boot_cortex_m` with caller-composed `SoCPeripherals::attach_all`.
Target Files: `.reports/baremetal-fw-readiness/{SUMMARY.md,03-micro.md,evidence.md,probe_results.json,probe_sources.md}`.
Callers: CLI `src/main.cpp:242-289`; acceptance `tests/baremetal_firmware_test.cpp:366-470`.
Glossary Additions: Firmware load mode (PT_LOAD physical address; startup handles separate VMA zero-fill); vector reset (MSP/PC from VTOR).
Refactor Strategy: Expand–Migrate–Contract.
