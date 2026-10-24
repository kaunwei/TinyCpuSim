# TASK-038 probe evidence

## Inputs and bounds

- Original spec read: `/home/kw/workspace/TinyCpuSim/.scratch/baremetal-isa-completeness/spec.md` (TASK-034/035/036 details).
- Requested `/home/kw/workspace/TinyCpuSim/.reports/spec-implementation-audit/recent-specs.md` was not present.
- Product and test sources were read-only. No gem5 calibration performed; no uarch implementation conclusions made.
- Current base: `4026514975bf33dfa32e52d59b32c1c3624a49ef`.

## Build/runtime checks

- `/usr/bin/cmake --build build -j2`: `ninja: no work to do` (the pre-existing build tree already reflects current source; does not prove a fresh compile occurred).
- `./build/baremetal_firmware_test --gtest_filter=BaremetalFirmwareTest.FullCortexMBootAndBareMetalInstructionSuiteWithPrimask`: 1 test passed. It directly checks vector MSP/PC, CPSID/CPSIE masking, SUB/ADD SP, PUSH.W/POP.W, UDIV, UXTB, REV and DMB; program bytes are manually supplied with `bus.write16` in `tests/baremetal_firmware_test.cpp:271-379`.
- `/usr/bin/ctest --test-dir build --output-on-failure`: 264/264 passed. This broad result is recorded as a repository check, not evidence for excluded engine behavior.
- `command -v arm-none-eabi-gcc`, `arm-none-eabi-ld`, `clang`, `ld.lld` and explicit `/usr/bin` probes returned no paths. The existing firmware Makefile (`firmware/Makefile:1-11`) defaults to arm-none-eabi GCC, `-mcpu=cortex-m3 -mthumb -nostdlib -O2 -Wall`, and silently skips its ELF build if absent. No sudo/package installation attempted.

## Focused current-source probe

Temporary `/tmp/task038_probe.cpp` included current `tinyarmsim/interpreter.hpp`, compiled with `/usr/bin/g++ -std=c++20 -Wall -Wextra -Iinclude`, and ran without warnings:

```text
e920 3 op=STM rn=0 ... wb=1 pre=1 add=1 ... imm?=0
e890 3 op=LDM rn=0 ... wb=0 ...
f990 1 op=LDRB rn=0 ... size=1 signed=0 ...
f9b0 1 op=LDRH rn=0 ... size=2 signed=0 ...
f850 d04 op=LDR ... wb=1 pre=1 add=0 imm=4 imm?=1
f850 b04 op=LDR ... wb=1 pre=0 add=1 imm=4 imm?=1
f850 403 op=LDR ... pre=1 add=1 imm=403 imm?=1
STMDB runtime base=208 mem200=11111111 mem1fc=0 mem1f8=0
```

The last input was `STMDB.W R0!,{R1,R2}` (`E920 0006`), with initial `R0=0x200`, `R1=0x11111111`, `R2=0x22222222`; observed `R0=0x208`, first store at `0x200`, no stores below base. The architecture-consistent expected result is base `0x1F8`, stores at `0x1F8` and `0x1FC` in ascending register order. `F990 0001` / `F9B0 0001` are signed byte/halfword immediate forms, but observed decoded `is_signed_mem=false` and unsigned opcodes.

The `F850` post/pre-indexed examples show that negative subtraction (`add=false`), pre/post selection, and writeback bits are carried into the decoded struct for these input encodings. The `F850 0403` sample was not an encoded register-offset exemplar (immediate path); do not infer register-offset runtime coverage from it. Decoder source `include/tinyarmsim/decoder.hpp:695-712` explicitly recognizes shifted register offset, but no focused runtime check was run for that encoding.

## Source/test anchors and boot boundary

- Decode Thumb-1 families: `include/tinyarmsim/decoder.hpp:336-407`; execute: `include/tinyarmsim/interpreter.hpp:589-665`.
- Divide decode: `decoder.hpp:590-598`; division execution: `interpreter.hpp:637-651`.
- Wide multi-transfer decode: `decoder.hpp:474-489`; shared incrementing transfer executor: `interpreter.hpp:538-571`.
- Wide B.W decode: `decoder.hpp:645-680`; common condition and branch execution: `interpreter.hpp:67-92,422-427`.
- Wide single transfer decode: `decoder.hpp:683-725`; address calculation: `interpreter.hpp:465-501`. The decoder computes LDRSB/LDRSH op class nowhere in this wide path and leaves `is_signed_mem` default false.
- PRIMASK reset/accessors: `state.hpp:28-32,87-101`; CPS execution `interpreter.hpp:653-659`; NVIC gates entry at `nvic.hpp:118-125`.
- Loader maps PT_LOAD bytes to `p_vaddr` at `loader.hpp:85-98`, then vector reset sets MSP/PC at `loader.hpp:106-112`. Linker assigns `.data` RAM VMA with `AT(_etext)` at `firmware/bsp/linker.ld:24-31`; startup copies `_etext` to `_sdata.._edata`, then clears `_sbss.._ebss` at `firmware/bsp/startup.s:37-60`.
- Existing `tests/loader_test.cpp:83-104` validates ELF loading and zero-fill when VMA equals LMA; it does not exercise distinct `.data` LMA/VMA. Existing bare-metal boot acceptance does not invoke Loader or startup assembly.
- Existing focused test `tests/system_control_and_svc_test.cpp:99-~225` covers Thumb-1 extensions/PRIMASK and a Thumb-2 divide/branch/stack/indexed-transfer suite; existing bare-metal test exact range is `tests/baremetal_firmware_test.cpp:271-379`.
