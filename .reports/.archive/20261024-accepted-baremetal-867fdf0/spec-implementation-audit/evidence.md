# TASK-037 detailed evidence

## Examination boundary

- Exact source snapshot: `/home/kw/workspace/TinyCpuSim/.worker-tree`, HEAD `437362fe314e7be57bf6c23231fef84cd6395678`, branch `feature/worker-frontier`; HEAD subject `refactor(mmio,loader): add PRIMASK gating and Cortex-M boot (TASK-036)`.
- Pre-existing dirty status preserved: deleted `.reports/baremetal-fw-mmio-discovery/SUMMARY.md`, `.reports/baremetal-isa-gaps/{SUMMARY.md,03-micro.md}`, `.reports/mmio-perf-timer-clock/{SUMMARY.md,03-micro.md}`; untracked `.reports/.archive/`. These are excluded from current evidence. No task target existed before report generation.
- Read all 4 active `spec.md` files and 39 issue files under `.scratch`; reviewed bounded recent history (20 HEAD commits, plus relevant feature commits `012be72`, `e732fa5`, `be78a4b`, `b80a976`, `6dd3301`, `078e77f`, `ddaf33f`, `8ecf79f`, and earlier roadmap commits). Runtime `tasks.done` and `progress.log` reside outside isolated tree; reviewed latest available records/tails. Do not equate historical DONE with current acceptance.
- No current report was reused; old dirty/deleted/archive reports do not establish current facts.

## SPEC clause matrix

### `.scratch/tinyarmsim-core/spec.md` — partial, stale-documentation

Implemented evidence: `include/tinyarmsim/state.hpp`, `memory_bus.hpp`, `decoder.hpp`, `interpreter.hpp`, `loader.hpp`; tests include `state_and_memory_test`, `loader_test`, ISA tests, assembly self-test and firmware test. Test fixtures exist under `tests/fixtures/`. Current CTest reports 264 passing cases. Clause gaps: the SPEC’s flat-memory/peripherals-out-of-scope and ~50–55 Thumb subset no longer describes shipped tasks 020–036 (MMIO, NVIC, Cortex-M boot, Thumb-2); test pass does not exhaustively establish every defined encoding or fault behavior. Classification is documentation drift plus unverified exhaustive coverage, not missing implementation by filename.

### `.scratch/opcode-cache-and-spike-log/spec.md`, issues 01–04 — partial

- Issue 01/disassembly: implemented; decoder adds precomputed fields; `tests/disassembler_test.cpp#L83-93` verifies two decoder-attached strings, plus format cases at `#L8-81`. The historical “all 45 opcodes” extent is not independently established by those cases.
- Issue 02/cache: `OpcodeCache` raw-keyed 64K 16-bit table and raw 32-bit word map (`include/tinyarmsim/opcode_cache.hpp#L17-39`); interpreter calls it (`interpreter.hpp#L694-707`). Required exact behavior “modifying bytes at any PC immediately executes new opcode” lacks a registered targeted same-PC mutation test in inspected tests. Cache identity design suggests PC-independent decoding, but observed source does not itself prove stale entries cannot occur under all fetch/cache paths.
- Direct dispatch criterion is **missing**: class stores decoded records, not a dedicated execution function pointer; `DecodedInstruction` carries opcode data and interpreter execution remains through its existing switch-style semantics. “~80 MIPS” historical resolved issue says 73.79 MIPS once on Release, but current invocation did not rerun that benchmark.
- Issue 03 trace: trace fields/format are implemented (`trace.hpp#L24-82`), hooks are gated on logging (`interpreter.hpp#L99-105`, `#L118-157`, `#L796-805`), and registered tests check substrings (`tests/trace_test.cpp#L9-43`). Exact golden-line equality and a quantitative zero-cost disabled mode are unverified; only branch guarding is visible.
- Issue 04 stress: `tests/asm/test_stress.s` and fixture present, and CTest test executes it. This is source/registered test evidence, not current throughput measurement.

### `.scratch/perf-calibration-ubench/spec.md`, issues 01–06 — partial/unverified calibration

Current test registration contains isolated BPU/front-end, exec/LSU, ROB/topdown, cache/coherence and Store Sets/fusion/LSD suites. Reporter was executed and its comparisons passed at the stored thresholds; CTest independently passed 264/264. The reporter labels oracle values `gem5`, but `tests/uarch/golden_counters.json#4` points to `/Users/kuanwei/workspace/gem5`, has embedded `gem5_val` constants, and does not point to raw run/config/ELF artifacts. Thus observed PASS means code output agrees with embedded values; empirical origin is unverifiable from repository evidence. Most listed tolerance is 0–1%, but isolated counters are not equivalent to an end-to-end cycle/IPC/cache-miss <1% gate. `scripts/compare_with_gem5.py` can run against `tests/golden/gem5/*.stats.txt`; this audit did not run it, and checked-in reports do not show a successful current same-model gem5 campaign. `scripts/report_ubench_perf.py --suite all` output included exact invariant matches and examples 2009/2000 (+0.45%) and 1050/1045 (+0.48%); these are measured TinySim values compared with stored values, not verified fresh gem5 outputs.

### `.scratch/uarch-ooo-engine/spec.md` and Issues 01–12 — partial

- Issues 01–06: config/stats, caches/MESI, BPU/frontend, RAT/PRF, ROB/IQ, LSU have source modules and registered focused tests. Dual issue and full concurrency semantics beyond specific unit fixtures are not generally established by source presence.
- Issue 07 integrated core: `OoOCore` and end-to-end arithmetic/fibonacci/store-forward/stress tests exist and pass. Acceptance asks branch loops and memory sort on OoO, and intentional mispredict result validation; those exact cases should be checked separately (the current registered set alone does not prove every requested workload).
- Issue 08 multicore: `MultiCoreSystem` lockstep ticking and aggregate stats exist (`multicore_system.hpp#L51-64`, `#L85-120`). Registered test only runs four isolated code regions (`tests/uarch/multicore_system_test.cpp#L7-40`); shared L2/cache test exists, but no robust concurrent shared workload or explicit 1/2/4/8 instantiation sweep was found. Bus arbitration acceptance remains unverified.
- Issue 09 CLI/end-to-end: `src/main.cpp` includes a uArch path and current test suite exercises uArch binaries, but exact requested preset filenames and complete CLI structured-stat generation require a dedicated CLI probe; not run here.
- Issue 10 MPIDR/SMP/AMP/atomics: uArch coherence has exclusive monitor methods (`include/tinyarmsim/uarch/coherence.hpp#L241-249`) and `coherence_test` contains `ExclusiveMonitorLdrexStrex` (`tests/uarch/coherence_test.cpp#L67-87`). This is an isolated monitor test, not a parallel workload under high contention. Core functional `ArchitecturalState`/interpreter has no inspected MPIDR implementation; explicit multi-ELF CLI and per-core stack setup were not located. Clause missing/unverified.
- Issue 11 golden precision: comparison tools and checked-in gem5 stats exist, but identical workload/configuration and stated 5–10% precision are not established by fresh comparison in this run. The stronger under-1% criterion in worker notes is likewise not established as empirical.
- Issue 12 topdown: `topdown_profiler.hpp`, ROI handling and tests exist; CTest + named isolated cases pass. The old issue answer’s count “110/110” is stale relative to current 264 CTest cases.

## Current claims: MMIO and ISA

Tasks 020–036 are visible in bounded HEAD history. `MMIODevice` interface is at `include/tinyarmsim/mmio/device.hpp#L14-30`; `ClockedMMIODevice#L33-79` owns countdown/OVR behavior. `SoCPeripherals::attach_all` constructs/registers devices and attaches NVIC (`include/tinyarmsim/mmio/soc_platform.hpp#L32-65`). Recent interpreter/wiring changes and the one end-to-end firmware case pass in CTest. `Decoder::decode32_internal` has wide load/store, multiple, divide decode branches (`decoder.hpp#L474-489`, `#L590-598`); however one fixture does not establish completeness across all Cortex-M instructions, exception cases, or register maps. Earlier gap reports are missing from the active tree and archived reports are not accepted as evidence.

## Verified callers and public seams

- `IsaInterpreter::step()` fetches/decode through `OpcodeCache::get_16/get_32`: `include/tinyarmsim/interpreter.hpp#L694-707`.
- `IsaInterpreter` records register/memory deltas only when logging enabled: `interpreter.hpp#L99-105`, `#L118-157`; emits `TraceRecord::format()` at `#L796-805`.
- `MultiCoreSystem::tick()` calls each live core’s `tick()` (`multicore_system.hpp#L51-64`); `run()` repeats to halt or cycle limit (`#L66-83`).
- `SoCPeripherals::attach_all` is a public assembly seam (`soc_platform.hpp#L32-65`); used in firmware test; test calls verify whole configured platform execution.
- Decoder public surface exists in `decoder.hpp`; inspected implementation slice for UDIV/SDIV at `#L590-598`.

## Probe results and limitations

- Ran `/usr/bin/python3 scripts/report_ubench_perf.py --suite all`: output reports stored-counter comparisons; visible rows PASS, including +0.45% and +0.48%; no raw gem5 process was run by this command.
- Ran `ctest --test-dir build --output-on-failure`: **264/264 passed**, 0 failures, 1.68 sec. This uses existing build outputs (many timestamps Oct 23); it is not a clean rebuild and does not prove current source recompiles. No warnings claim is made.
- `VERIFY: REPORT_ONLY`: no compile, product edits, test edits, compiler signature mutation, new ticket, or gem5 run.
- No claims of health are made for excluded subsystems. All `path#Lline` anchors above refer to examined HEAD; report becomes stale if source paths change.
