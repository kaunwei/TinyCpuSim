<!-- REPORT_META: Task=TASK-038 | Commit=4026514975bf33dfa32e52d59b32c1c3624a49ef -->
## Scope and result

Fresh TASK-034/035/036 audit of the original spec for `IsaInterpreter` only. Most listed paths exist; focused executable probe found two defects: `STMDB.W` acts as incrementing STM, and wide `LDRSB`/`LDRSH` decode unsigned. Compiled-C startup remains unverified; ELF LMA loading is a gap. No full ARMv7-M or Perf Mode claim.

The requested `/home/kw/workspace/TinyCpuSim/.reports/spec-implementation-audit/recent-specs.md` was absent. Earlier reports are archived context only. Required helper-generated skeletons/slices are in [03-micro.md](03-micro.md).

## Topology and status

`IsaInterpreter::step` fetches through `Decoder` then dispatches `DecodedInstruction` to `execute` ([step](../../include/tinyarmsim/interpreter.hpp#L694), [decode](../../include/tinyarmsim/decoder.hpp#L17), [execute](../../include/tinyarmsim/interpreter.hpp#L94)). State owns NZCV/PRIMASK ([state](../../include/tinyarmsim/state.hpp#L22)); NVIC gates exceptions ([nvic](../../include/tinyarmsim/mmio/nvic.hpp#L118)). Loader maps ELF and reads vector reset ([loader](../../include/tinyarmsim/loader.hpp#L29)); startup assembly initializes C data ([startup](../../firmware/bsp/startup.s#L31)).

- Thumb-1 SP/ADR/extend/reverse/CPS/hints decode at `decoder.hpp:336-407`, execute at `interpreter.hpp:589-665`. System-control tests pass. WFE aliases WFI; SEV/NOP are no-ops in this effect-free model.
- Divide/B.W/barrier/wide transfer decode at `decoder.hpp:474-489,590-620,645-725`; existing Thumb-2 unit test passes. STMDB loses decrement direction. Wide signed loads leave `is_signed_mem=false`. Negative/pre/post immediate fields decode; register offset has source path but no focused runtime probe.
- Conditions and PRIMASK are implemented at `interpreter.hpp:67-92,653-659`, `nvic.hpp:118-125`. Boot/PRIMASK test passes; direct broad B.W condition acceptance is missing.
- `boot_cortex_m` already loads MSP/reset PC; TASK-036 calls this `boot_from_vector_table`, so settle the public name. Startup copies `.data`/clears `.bss`, but current manual test does not execute it. ELF loader writes PT_LOAD at `p_vaddr` only (`loader.hpp:85-98`); linker gives `.data` RAM VMA and Flash `AT(_etext)` (`linker.ld:24-31`), so Flash LMA initialization is unproven.

## Bounded vertical slices

| Slice / targets | Red → green observation |
|---|---|
| A. `instruction.hpp`, `decoder.hpp`, `interpreter.hpp`, `tests/load_store_and_stack_test.cpp` | `E920 0006` observed R0 `0x208`, store at `0x200`, none below. Decode P/U/W; store R1,R2 at `base-8,base-4`, write back `base-8`; preserve legacy STM/LDM/PUSH/POP. |
| B. `decoder.hpp`, `tests/load_store_and_stack_test.cpp` | `F990 0001` -> unsigned LDRB; `F9B0 0001` -> unsigned LDRH. Decode signed forms and verify `0xFFFFFF80`/`0xFFFF8001`; retain negative/pre/post/register addressing. |
| C. `loader.hpp`, `startup.s`, `linker.ld`, `firmware/Makefile`, `tests/baremetal_firmware_test.cpp`, C fixture | Current manual test cannot validate startup; loader ignores `p_paddr`. Build Cortex-M3 C fixture, load LMA to Flash, vector boot/run startup/main; assert `.data`, `.bss`, result/SVC. Toolchain absent here. |
| D. `tests/system_control_and_svc_test.cpp`, `tests/baremetal_firmware_test.cpp` | Add B.W positive/negative taken/untaken and NZCV preservation; check pending IRQ stays masked until CPSIE. |

**Expand–Migrate–Contract:** Expand A/B red tests and compiled fixture; migrate decoder fields/executor while current legacy tests stay green; contract duplicate helpers only after behavior passes. C integrates LMA/vector/startup; D adds acceptance only. No interface signature change indicated beyond resolving the boot method name.

## Verification

Build: no work to do. Exact boot acceptance: 1/1 passed. CTest: 264/264 passed (broad repository check, not Perf evidence). Temporary current-header probe compiled warning-free with `/usr/bin/g++ -std=c++20 -Wall -Wextra`; details/tool availability in [evidence.md](evidence.md). No product/test edits. `VERIFY: REPORT_ONLY`.
