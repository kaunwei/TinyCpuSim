<!-- REPORT_META: Task=TASK-037 | Commit=437362fe314e7be57bf6c23231fef84cd6395678 -->
# SPEC implementation audit

Examined HEAD `437362fe314e7be57bf6c23231fef84cd6395678` in `/home/kw/workspace/TinyCpuSim/.worker-tree` (`feature/worker-frontier`). Read all four active specs and 39 issue files under `.scratch`, bounded history (20 HEAD commits plus feature commits cited in evidence), runtime `tasks.done`/`progress.log` from the human checkout, implementation and registered tests. Existing unrelated dirty state was preserved: five deleted older report paths and untracked `.reports/.archive/`; neither used as current evidence. No product/test edits.

## Per-SPEC status

| SPEC | Status | Exact unmet or unverified clauses |
|---|---|---|
| `tinyarmsim-core` | **stale-documentation; coverage unverified** | Current shipped MMIO/Cortex-M/Thumb-2 work contradicts “peripherals out of scope” and the old ~50–55 instruction boundary. Tests do not prove every listed encoding/fault case. |
| `opcode-cache-and-spike-log` | **partial** | No dedicated handler/function-pointer dispatch; no registered same-PC self-modifying-code test; ~80 MIPS not remeasured. Trace tests check substrings, not exact whole-line/disabled-cost criteria. Historical 73.79 MIPS is not current evidence. |
| `perf-calibration-ubench` | **partial; calibration provenance unverified** | Stored golden values compare successfully, but `golden_counters.json` points to `/Users/kuanwei/workspace/gem5` and lacks raw run/config/ELF provenance. Reporter PASS is agreement with embedded constants, not fresh gem5. End-to-end <1% cycles/IPC/cache-miss gate is not established. |
| `uarch-ooo-engine` | **partial** | Modules and focused tests exist. Integrated branch/mispredict workload matrix, shared multi-core contention/arbitration, 1/2/4/8 sweep, MPIDR/AMP/stack contract, atomic contention, fresh gem5 5–10% validation, and CLI preset/export clauses remain unverified or missing. Issue 12 profiler/ROI has focused tests. |

Recent TASK-034–036 ISA/MMIO claims are supported by source and one passing full-firmware fixture, not exhaustive Cortex-M/MMIO completeness. Older gap reports are absent from active tree; archive is excluded.

## Prioritized unfinished slices

1. Reproducible gem5 golden provenance and generated data; run same ELF/config and enforce measured <1% metric gate.
2. Opcode-cache same-PC byte replacement test and resolve the missing direct-handler dispatch/performance requirement.
3. Multi-core shared workload with contention/arbitration and explicit core-count matrix; implement or explicitly retire MPIDR/AMP claims.
4. Reconcile living ISA/uArch SPEC boundaries and map supported/unsupported clauses to tests.

## Topology and strategy

`Decoder → DecodedInstruction → OpcodeCache → IsaInterpreter` is the functional path; `UOpDecoder → OoOCore → ROB/IQ/LSU → MultiCoreSystem → CoherentMemoryHierarchy` is the cycle model. `MemoryBus` and config cross seams. Coupling bottlenecks: duplicated functional/uArch semantics, golden constants detached from the gem5 runner, and loader/peripheral/NVIC wiring at firmware setup.

| Slice | Expand | Migrate | Contract |
|---|---|---|---|
| Golden | Preserve fields, add run/config/ELF provenance | Generate comparisons from reproducible gem5 outputs | Remove unsupported calibrated labels after consumers move |
| Cache | Add mutation/dispatch baselines | Validate interpreter fetch through cache contract | Retire duplicate path only with parity/perf data |
| Multicore | Define core identity and workload evidence | Integrate setup/workload through coordinator/coherence | Narrow unsupported roadmap claims after tests/docs agree |
| ISA/MMIO | Enumerate shipped paths | Map each clause to tests | Mark unsupported clauses explicitly |

## Seam digest

Seam: `Decoder::decode16/decode32`, `OpcodeCache::get_16/get_32`, `IsaInterpreter::step`, `TraceRecord::format`; uArch `MultiCoreSystem::tick/run`; MMIO `MMIODevice`, `SoCPeripherals::attach_all`.

Target Files: `.reports/spec-implementation-audit/{SUMMARY.md,03-micro.md,evidence.md}`. Callers: interpreter calls cache; `src/main.cpp` selects engine; coordinator ticks cores; firmware test attaches full SoC. See evidence for verified `path#Lline`. Glossary: none. Strategy: Expand–Migrate–Contract for future seam work; no tickets created.
