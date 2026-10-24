<!-- REPORT_META: Task=TASK-051 | Commit=6f0f42e58686fd43c5c7ab7b0192121b02b965de -->
# Bare-metal firmware readiness

## Readiness matrix

| Probe | Outcome | Meaning |
| --- | --- | --- |
| Direct firmware test binary | 5/5 PASS | Existing MMIO, IRQ, GPIO/DMA, vector reset/context and compiled data/BSS/SVC acceptance. |
| Curated ELF through explicit board runner | rc0 | Firmware loader + SoC + VTOR/vector reset works for the curated fixture. |
| Public CLI curated ELF | exit/R0=1 after88 instructions | CLI uses default ELF load; no board/vector-reset composition. |
| Natural C, ordinary O0/O2 | **12 rows:3 pass,9 fail** | 5 wrong results (IT:1,NEGS:4),4 CMN.W faults; direct arguments avoid BSS/startup input loss. |
| Fresh unmodified BSP/demo, ordinary O2, explicit board runner | **PC0x10204 F04F4380 MOV.W r3,#0x40000000** | First ISA failure in uart_init after complete board/reset setup. |

Older /tmp/task051/demo.elf is secondary evidence only: board runner faults at0x101F8 F04F4280 (MOV.W r2,#0x40000000); public CLI reports the same encoding after61 instructions. Fresh demo-O2 was compiled from unchanged repository sources with the exact commands in probe_sources.md.

## Priority gaps and topology

1. **ITSTATE/predication:** O2 `x?7:9` uses BF0C ITE EQ and returns7 for input0 (expected9); input1 returns7 correctly. O0 select returns9/7 correctly. IT becomes NOP at include/tinyarmsim/decoder.hpp#L338.
2. **NEGS/RSB:** +/-128 both return0 at O0/O2 (425B/4240); decoder.hpp#L146 maps to generic RSB and interpreter.hpp#L270 subtracts register operands instead of0-input.
3. **CMN.W:** `x==-128` faults at O0PC0x1003E F1130F80 and O2PC0x10010 F1100F80 for both-128/-127. Later IT code is not reached in these probes.
4. **MOV.W immediate:** fresh ordinaryO2 BSP cannot materialize its UART base, as measured above.
5. **CLI board entry:** src/main.cpp#L242 constructs bus/state/interpreter; #L266 calls default Loader::load_elf, without SoC attach or vector reset. Firmware mode LMA loading is loader.hpp#L98; vector reset #L116. SoCPeripherals::attach_all (mmio/soc_platform.hpp#L31) owns device registration and NVIC hookup. Startup copies data/clears BSS and calls main/SVC (firmware/bsp/startup.s#L34).

The compiled acceptance composes these seams and checks symbol-based records at tests/baremetal_firmware_test.cpp#L367. Its O1/no-if-conversion flags (CMakeLists.txt#L272) are a bounded fixture contract, not ordinary compiler compatibility. These experiments do not establish general firmware or RTOS readiness. Calibration remains closed.

## Expand–Migrate–Contract matrix

| Seam | Expand | Migrate | Contract / gate |
| --- | --- | --- | --- |
| Decoder -> decoded instruction -> interpreter/state | Preserve ordinary compiled IT/NEGS/CMN/MOV examples as independent regressions | Implement each observable semantic family through existing seams | Remove silent/undefined fallbacks only after both-input tests pass; full functional regressions |
| CLI -> Loader -> SoC/vector boot | Add opt-in coherent firmware board entry while retaining generic ELF path | Exercise curated fixture and natural BSP through public entry | Retire duplicated composition only when data/BSS, reset/IRQ and SVC are covered |

## Seam digest

Seam: Decoder/DecodedInstruction/IsaInterpreter/ArchitecturalState; CLI/Loader/SoCPeripherals.
Target Files: include/tinyarmsim/{decoder.hpp,instruction.hpp,interpreter.hpp,state.hpp,loader.hpp}, src/main.cpp, include/tinyarmsim/mmio/soc_platform.hpp; research only, no implementation tickets.
Callers: ordinary compiler functions and firmware uart_init (measured); CLI src/main.cpp#L266; compiled firmware acceptance tests/baremetal_firmware_test.cpp#L367.
Glossary Additions: ITSTATE (conditional instruction block state); firmware load mode (physical/LMA placement).
Refactor Strategy: Vertical-Slice for ISA behavior; Expand–Migrate–Contract for public firmware composition.

03-micro.md contains actual human-helper extraction output plus exact ten-line source slices. probe_sources.md contains complete reproducible sources and commands; probe_results.json records all observations.
