---
name: uarch-perf-correlation
description: "Calibrate and align CPU microarchitectural performance counters against empirical golden references (e.g., gem5) with <1% error invariants using clean-room TDD microbenchmarks, isolated component telemetry, and strict regression gates."
---

# Microarchitectural Performance Correlation & Calibration

The `uarch-perf-correlation` skill defines the end-to-end engineering workflow for diagnosing, calibrating, and aligning cycle-accurate CPU microarchitectural performance counters against empirical golden architectural references (e.g., gem5) to achieve $<1\%$ error on isolated invariants while preserving 100% functional correctness.

This workflow integrates with the existing engineering skills: `tdd`, `codebase-design`, `diagnosing-bugs`, and `code-review`.

---

## 1. Core Principles

### 1.1 Empirical Ground-Truth Baseline (Oracle Discipline)
- **Golden References MUST Come from Real Execution, NOT Speculation**:
  - The comparison baseline must be derived from **actual empirical execution of the oracle simulator (gem5)** under identical microarchitectural configurations.
  - Arbitrary, subjective hand-wavy expectations are strictly prohibited because they lead to subtle timing and state machine drift.
  - Calibrated golden invariants are stored in the structured database: [`tests/uarch/golden_counters.json`](file:///Users/kuanwei/workspace/TinySim/tests/uarch/golden_counters.json).

### 1.2 Clean-Room Reference Discipline (gem5 只能參考不能抄)
- **gem5 is an Architectural Oracle / Theory Reference, NOT Code to Copy**:
  - **Strictly Prohibited**: Copy-pasting gem5 C++ classes, macros, or internal data structures into the codebase.
  - **Permitted & Encouraged**: Inspecting gem5 source code, `stats.txt`, formulas, state machine transitions, indexing math, event timing, and pipeline latency models to understand the theoretical hardware mechanism.
  - **Clean-Room Implementation**: Design idiomatic Modern C++20 domain structures (enums, RAII, strong typing, deep modules) that fit TinySim's architecture and pre-agreed seams.

### 1.3 Microbenchmark-Only Discipline (Layer A Focus)
- **Strict Scope Constraint**:
  - Microarchitectural performance calibration strictly uses **isolated subsystem microbenchmarks** (`tests/uarch/*_ubench_test.cpp`) and the automated comparator script ([`scripts/report_ubench_perf.py`](file:///Users/kuanwei/workspace/TinySim/scripts/report_ubench_perf.py)).
  - End-to-end full binaries (e.g. `test_stress.elf`) introduce external pipeline noise (instruction cache fills, unaligned multi-byte dispatch, OS SVC calls) that obscure isolated hardware invariants.
  - Subsystems (`bp`, `core`, `lsu`, `rob`, `cache`) are tested in isolation with zero external interference.

### 1.4 Standardized Telemetry Protocol
- Every microbenchmark test case emits real-time runtime counters to `stdout` in the standard telemetry format:
  ```cpp
  std::cout << "[PERF_COUNTER] <TestName>:<counter_key>=" << actual_value << std::endl;
  ```
- Each test case in GoogleTest runs with its own isolated hardware instance, ensuring independent life cycle and zero cross-test state leakage.

### 1.5 Multi-Buffer Pipeline Accounting Integrity
- For in-flight accounting (e.g., squashed speculative branches, flushed instructions, resource allocation counts):
  - Every hardware pipeline buffer that can hold speculative state must be audited during squash/flush:
    1. **Reorder Buffer (ROB)**: Younger speculative entries.
    2. **Execution / Writeback Buffers**: Uops that finished execution in the same cycle or are in transit to writeback.
    3. **Issue / Dispatch Queues & Reservation Stations**: Pending unissued/issued speculative uops.
    4. **Rename / Decode Queues**: Uops in transit between decode and rename.
    5. **Fetch / ICache Uop Queues**: Speculative uops fetched along the predicted path.
  - Missing any in-flight buffer causes systematic counter drift under high-bandwidth superscalar execution.

---

## 2. Automated Reporting & Comparison Tool (`scripts/report_ubench_perf.py`)

The comparator script reads the empirical golden cache, executes isolated microbenchmark binaries, parses real-time `[PERF_COUNTER]` telemetry, and enforces the $<1\%$ Invariant Delta gate.

### CLI Usage Examples:

```bash
# Evaluate all subsystem microbenchmarks across the entire CPU
python3 scripts/report_ubench_perf.py --suite all

# Evaluate Branch Predictor & Frontend (BPU) counters
python3 scripts/report_ubench_perf.py --suite bp

# Evaluate Execution Engine & Renaming (Core) counters
python3 scripts/report_ubench_perf.py --suite core

# Evaluate Load-Store Unit & Memory Disambiguation (LSU) counters
python3 scripts/report_ubench_perf.py --suite lsu

# Evaluate Reorder Buffer & Top-Down TMAM (ROB) counters
python3 scripts/report_ubench_perf.py --suite rob

# Evaluate Cache Hierarchy & MESI Coherence counters
python3 scripts/report_ubench_perf.py --suite cache

# Run only a specific test case using GoogleTest filter
python3 scripts/report_ubench_perf.py --suite bp --filter "*TightLoop*"

# Export side-by-side comparison table to GitHub Markdown
python3 scripts/report_ubench_perf.py --suite all --export-md perf_report.md
```

---

## 3. The 5-Phase Calibration Workflow

```
[Phase 1: UBench Counter Audit] ──► [Phase 2: Red Test (Isolated Invariant)]
               │                                       │
               ▼                                       ▼
[Phase 5: Dual-Axis Review & Commit] ◄── [Phase 4: Regression Gate] ◄── [Phase 3: Green Alignment (<1%)]
```

### Phase 1: Microbenchmark Performance Counter Audit
1. Run the subsystem ubench comparator:
   ```bash
   python3 scripts/report_ubench_perf.py --suite <subsystem>
   ```
2. Identify any counter with Delta $\ge 1.0\%$:
   - **Frontend / BPU (`--suite bp`)**: `BTB Hits`, `BTB Misses`, `Direct Cond / Uncond`, `Committed Mispredicts`, `Mispredict Due to Direction`, `Mispredict Due to BTB Miss`, `Squashed Spec Branches`, `RAS Returns Used`.
   - **Execution & Core (`--suite core`)**: `Raw Dependency Latency`, `Issue Queue Saturation`, `Age-Ordered Issue Priority`, `MUL/DIV Latencies`, `Execution Port Mapping`.
   - **LSU & Memory (`--suite lsu`)**: `Store-to-Load Forwarding Rate`, `Memory Order Violations`, `Replayed Loads`, `LSQ Wrap-around Capacity`.
   - **ROB & Top-Down (`--suite rob`)**: `Sustained Retire Throughput`, `Head-of-ROB Block Count`, `ROB Circular Wrap-around`, `Top-Down Slot Conservation Invariant (100%)`.
   - **Cache & MESI (`--suite cache`)**: `L1 Hit Latency (2 cycles)`, `N-way LRU Victim Selection`, `MSHR Concurrent Miss Limit`, `MESI Coherence Transitions`.

### Phase 2: Red Test — Isolated Component Microbenchmark
1. Add or extend the isolated microbenchmark in `tests/uarch/<subsystem>_ubench_test.cpp`.
2. Construct deterministic stimulus sequences isolating the subsystem.
3. Emit standard `[PERF_COUNTER]` telemetry.
4. Verify that the test fails (Red) or exposes the exact discrepancy before modifying production code.

### Phase 3: Green Implementation — Performance Counter Alignment (<1% Error)
1. Write the minimal clean-room fix in the subsystem header/source on pre-agreed seams.
2. Re-run `python3 scripts/report_ubench_perf.py --suite <subsystem>` until 100% passing (Verdict: `ALL PASS (<1% EMPIRICAL INVARIANT DELTA)`).

### Phase 4: Full Repository Regression Gate
1. Execute the entire test suite across all unit and integration tests:
   ```bash
   ctest --test-dir build --output-on-failure
   ```
2. **Zero Tolerance Gate**: 100% of all tests must pass cleanly.

### Phase 5: Dual-Axis Code Review & Local Commit
1. Ensure Modern C++20 compliance, RAII, and clean seam boundaries.
2. Create an atomic local git commit:
   ```bash
   git commit -m "feat(<subsystem>): calibrate <counter> performance alignment against gem5 <1% error"
   ```

---

## 4. SOP: How to Add a New Microbenchmark

When adding a new microarchitectural benchmark or validating a new config:

1. **Implement C++ Microbenchmark**:
   In `tests/uarch/<subsystem>_ubench_test.cpp`, write a new `TEST(SuiteTest, CaseName)`. At the end, output:
   ```cpp
   std::cout << "[PERF_COUNTER] CaseName:counter_key=" << actual_val << std::endl;
   ```
2. **Register in Empirical Database**:
   In `tests/uarch/golden_counters.json`, add the benchmark entry under the corresponding suite:
   ```json
   {
     "ubench": "CaseName",
     "metric": "Description of Microarchitectural Invariant",
     "counter_key": "counter_key",
     "gem5_counter": "gem5.stats.exact_counter_name",
     "gem5_val": 1000,
     "tolerance_pct": 1.0
   }
   ```
3. **Verify Alignment**:
   Run `python3 scripts/report_ubench_perf.py --suite <suite> --filter "*CaseName*"` to verify $<1\%$ alignment.
