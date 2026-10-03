# Branch Prediction Profiling Report

## 1. Executive Summary

This report provides an in-depth microarchitectural profiling analysis of the branch prediction workload [`tests/fixtures/test_branch_pred.elf`](file:///home/kw/workspace/TinyCpuSim/tests/fixtures/test_branch_pred.elf) on **TinyCpuSim** (Out-of-Order superscalar core).

The benchmark executes **10,321 instructions** spanning three distinct branch behavior phases:
- **Phase 1 (Alternating TNTN Pattern)**: 500 loop iterations testing standard two-state branch alternation.
- **Phase 2 (4-Step Periodic Pattern)**: 400 loop iterations exercising multi-step periodic branch predictability.
- **Phase 3 (Correlated Pattern)**: 300 loop iterations stressing cross-branch correlation and history dependency.

### Overall Benchmark Statistics
- **Total Committed Instructions**: 10,321
- **Total Simulated Cycles**: 14,435 ticks (at 1000 MHz)
- **Overall IPC**: 0.715
- **Total Branch Predictions**: 5,104
- **Total Branch Mispredictions**: 808
- **Overall Prediction Accuracy**: 84.17%
- **L1I Hit Rate**: 99.97% (11,104 / 11,107)

---

## 2. Per-Phase Instruction Mix & Branch Breakdown

| Metric / Phase | Phase 1 (TNTN Pattern) | Phase 2 (4-Step Pattern) | Phase 3 (Correlated Pattern) | Total / Combined |
| :--- | :--- | :--- | :--- | :--- |
| **Instruction Count** | 3,758 (36.4%) | 3,106 (30.1%) | 3,457 (33.5%) | 10,321 (100.0%) |
| **Total Branches** | 1,252 (33.32% mix) | 1,101 (35.45% mix) | 1,051 (30.40% mix) | 3,404 (32.98% mix) |
| **Conditional Branches** | 1,002 | 801 | 901 | 2,704 |
| ↳ *Cond Taken* | 749 (74.75%) | 499 (62.30%) | 599 (66.48%) | 1,847 (68.31%) |
| ↳ *Cond Not Taken* | 253 (25.25%) | 302 (37.70%) | 302 (33.52%) | 857 (31.69%) |
| **Unconditional Branches** | 250 (`b`) | 300 (`b`) | 150 (`b`) | 700 (`b`) |
| **Arithmetic / Logic** | 2,004 (`movs`, `cmp`, `ands`, `adds`, `subs`) | 1,903 (`movs`, `cmp`, `ands`, `subs`, `adds`) | 2,103 (`movs`, `cmp`, `ands`, `subs`, `adds`) | 6,010 |
| **Control / Other** | 2 (`movw`) | 102 (`nop`, `movw`) | 153 (`nop`, `movw`, `svc`) | 257 |

### Phase-Specific Opcode Breakdown

```
Phase 1:
  - movs : 502 (13.4%)
  - cmp  : 502 (13.4%)
  - bne  : 502 (13.4%)
  - ands : 500 (13.3%)
  - beq  : 500 (13.3%)
  - adds : 500 (13.3%)
  - subs : 500 (13.3%)
  - b    : 250 (6.7%)
  - movw : 2 (0.1%)

Phase 2:
  - movs : 402 (12.9%)
  - cmp  : 401 (12.9%)
  - bne  : 401 (12.9%)
  - ands : 400 (12.9%)
  - beq  : 400 (12.9%)
  - subs : 400 (12.9%)
  - adds : 300 (9.7%)
  - b    : 300 (9.7%)
  - nop  : 100 (3.2%)
  - movw : 2 (0.1%)

Phase 3:
  - movs : 602 (17.4%)
  - cmp  : 601 (17.4%)
  - ands : 600 (17.4%)
  - bne  : 451 (13.0%)
  - beq  : 450 (13.0%)
  - subs : 300 (8.7%)
  - adds : 150 (4.3%)
  - b    : 150 (4.3%)
  - nop  : 150 (4.3%)
  - movw : 2 (0.1%)
  - svc  : 1 (0.0%)
```

---

## 3. Microarchitectural Performance & Slice Analysis

Using periodic 500-instruction interval slicing (`--slice-insts 500`), the dynamic behavioral transitions across phases are clearly separated:

| Phase Region | Slice Interval | Inst Window | Avg Slice IPC | Total Predictions | Mispredictions | Branch Accuracy |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Phase 1 (TNTN)** | Slice 0 – 6 | 0 -> 3505 | **0.773** | 1,638 | 236 | **85.59%** |
| **Phase 1 -> 2 Transition** | Slice 7 | 3505 -> 4007 | 0.930 | 230 | 27 | 88.26% |
| **Phase 2 (4-Step)** | Slice 8 – 12 | 4007 -> 6507 | **1.246** | 1,127 | 81 | **92.81%** |
| **Phase 2 -> 3 Transition** | Slice 13 | 6507 -> 7010 | 0.872 | 239 | 31 | 87.03% |
| **Phase 3 (Correlated)** | Slice 14 – 20 | 7010 -> 10321 | **0.489** | 1,870 | 433 | **76.84%** |

### Per-Slice Granular Trace (500 Insts / Snapshot)

- **Slice #0** [0 -> 500]: IPC 0.673 | BP: 234, Mispredicts: 35 (85.0% Acc)
- **Slice #1** [500 -> 1000]: IPC 0.792 | BP: 235, Mispredicts: 34 (85.5% Acc)
- **Slice #2** [1000 -> 1501]: IPC 0.786 | BP: 232, Mispredicts: 33 (85.8% Acc)
- **Slice #3** [1501 -> 2001]: IPC 0.789 | BP: 235, Mispredicts: 33 (86.0% Acc)
- **Slice #4** [2001 -> 2503]: IPC 0.797 | BP: 233, Mispredicts: 34 (85.4% Acc)
- **Slice #5** [2503 -> 3004]: IPC 0.784 | BP: 234, Mispredicts: 33 (85.9% Acc)
- **Slice #6** [3004 -> 3505]: IPC 0.791 | BP: 235, Mispredicts: 34 (85.5% Acc)
- **Slice #7** [3505 -> 4007]: IPC 0.930 | BP: 230, Mispredicts: 27 (88.3% Acc) *(Phase 1 End / Phase 2 Warmup)*
- **Slice #8** [4007 -> 4507]: IPC 1.247 | BP: 226, Mispredicts: 16 (92.9% Acc)
- **Slice #9** [4507 -> 5007]: IPC 1.244 | BP: 225, Mispredicts: 16 (92.9% Acc)
- **Slice #10** [5007 -> 5507]: IPC 1.247 | BP: 226, Mispredicts: 16 (92.9% Acc)
- **Slice #11** [5507 -> 6007]: IPC 1.244 | BP: 225, Mispredicts: 16 (92.9% Acc)
- **Slice #12** [6007 -> 6507]: IPC 1.247 | BP: 225, Mispredicts: 17 (92.4% Acc)
- **Slice #13** [6507 -> 7010]: IPC 0.872 | BP: 239, Mispredicts: 31 (87.0% Acc) *(Phase 2 End / Phase 3 Warmup)*
- **Slice #14** [7010 -> 7510]: IPC 0.486 | BP: 283, Mispredicts: 65 (77.0% Acc)
- **Slice #15** [7510 -> 8010]: IPC 0.490 | BP: 284, Mispredicts: 65 (77.1% Acc)
- **Slice #16** [8010 -> 8511]: IPC 0.492 | BP: 282, Mispredicts: 65 (77.0% Acc)
- **Slice #17** [8511 -> 9012]: IPC 0.489 | BP: 285, Mispredicts: 66 (76.8% Acc)
- **Slice #18** [9012 -> 9512]: IPC 0.486 | BP: 282, Mispredicts: 65 (77.0% Acc)
- **Slice #19** [9512 -> 10012]: IPC 0.490 | BP: 283, Mispredicts: 66 (76.7% Acc)
- **Slice #20** [10012 -> 10321]: IPC 0.494 | BP: 171, Mispredicts: 41 (76.0% Acc)

---

## 4. Key Architectural Insights

1. **Phase 2 (4-Step Periodic)** exhibits the highest throughput (**IPC 1.246**) and best predictor accuracy (**92.81%**). The local pattern repeating every 4 steps allows the two-bit bimodal/tournament predictor table to saturate and reliably predict loop tails and inner conditions.
2. **Phase 1 (Alternating TNTN)** achieves moderate throughput (**IPC 0.773**, **85.59%** accuracy). Rapid alternating conditions create some thrashing in 2-bit saturating counters, but loop-closing branches maintain steady behavior.
3. **Phase 3 (Correlated Branches)** is the most challenging phase, dropping throughput to **IPC 0.489** with an error rate of ~23.16% (**76.84%** accuracy). Because decision branches in Phase 3 depend on correlation between bit 0 and bit 1 of the counter register, local bimodal predictors experience higher misprediction penalties and pipeline flushes.
