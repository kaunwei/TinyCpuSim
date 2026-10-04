# Microarchitectural Profile Analysis: Integer Arithmetic & RAW Data Hazards

## Executive Summary
This report presents a detailed microarchitectural and execution-port contention analysis of the integer arithmetic benchmark (`tests/fixtures/test_arithmetic.elf`) and the back-to-back RAW data hazard benchmark (`tests/fixtures/test_raw_hazard.elf`) executed on **TinyCpuSim** (Out-of-Order superscalar core model with 4-wide dispatch/retire) and compared against empirical baseline stats from **gem5** (`tests/golden/gem5/test_arithmetic.stats.txt` and `test_raw_hazard.stats.txt`).

Both benchmarks evaluate pure computational pipelines with zero data-memory accesses (`0 Loads / 0 Stores`), isolating the issue queue (RS), physical register file (PRF), rename engine, and Execution Functional Unit (FU) dispatch ports (2 Simple Int ALU + 1 Complex Int Mult/Div FU vs. gem5 default FU pool).

---

## 1. Pipeline Telemetry & gem5 Baseline Comparison

### 1.1 `test_arithmetic.elf` (Arithmetic & Logic Operations Benchmark)

| Metric | TinyCpuSim (OoO Core) | gem5 Golden Baseline | Delta / Analysis |
| :--- | :---: | :---: | :--- |
| **Committed Instructions** | 3,807 | 3,805 | +2 insts (startup / svc termination) |
| **Committed uOps** | 3,807 | 3,805 (macro ops) | uOp expansion ratio: 1.00x |
| **Simulated Cycles** | 2,508 ticks | 1,760 cycles | Difference due to 2 ALU issue limit vs gem5 6-ALU pool |
| **Core IPC** | 1.518 inst/cycle | 2.162 inst/cycle | gem5 issues up to 6 ALUs concurrently |
| **Core CPI** | 0.659 cycle/inst | 0.463 cycle/inst | +0.196 cycle/inst |
| **Branch Predictions** | 201 | 201 | Exact match on branch loop count |
| **Branch Mispredicts** | 2 (Accuracy: 99.0%) | 2 | Mispredicts at initialization & final loop exit |
| **L1I Cache Hit Rate** | 99.95% (2188/2189) | 100.0% | Tight 55-byte loop footprint wholly inside L1I |
| **L1D Cache Hit Rate** | N/A (0 loads/0 stores)| N/A (0 accesses) | Pure register-to-register computation |

### 1.2 `test_raw_hazard.elf` (Back-to-Back Dependent Accumulate Chain)

| Metric | TinyCpuSim (OoO Core) | gem5 Golden Baseline | Delta / Analysis |
| :--- | :---: | :---: | :--- |
| **Committed Instructions** | 3,008 | 3,006 | +2 insts |
| **Committed uOps** | 3,008 | 3,006 | 1.00x uOp mapping |
| **Simulated Cycles** | 4,104 ticks | 4,325 cycles | Delta: -5.1% (closely aligned latency chain) |
| **Core IPC** | 0.733 inst/cycle | 0.695 inst/cycle | Dependent RAW serialization bottleneck |
| **Core CPI** | 1.364 cycle/inst | 1.439 cycle/inst | Highly serialized dependency chain CPI > 1.0 |
| **Branch Predictions** | 501 | 501 | 500-iteration loop + 1 exit check |
| **Branch Mispredicts** | 2 (Accuracy: 99.6%) | 2 | Precise loop termination branch handling |
| **L1I Hit Rate** | 99.97% (2965/2966) | 100.0% | Loop fits in 1 cache line (34B) |
| **L1D Hit Rate** | N/A (0 loads/0 stores)| N/A (0 accesses) | 0 memory traffic |

---

## 2. Functional Unit (FU) Pool & Port Dispatch Contention

### 2.1 Functional Unit Pool Architectures

- **gem5 Default FU Pool (`FUPool.py` / `FuncUnitConfig.py`)**:
  - `IntALU`: 6 units (OpClass: `IntAlu`, latency = 1 cycle).
  - `IntMultDiv`: 2 units (OpClass: `IntMult` latency = 3 cycles, `IntDiv` latency = 20 cycles non-pipelined).
  - Issue width: Up to 8 instructions per cycle.

- **TinyCpuSim Execution Ports (`ooo_core.hpp`)**:
  - `Port 0 & Port 1 (Simple Int ALU)`: Handles `ADD`, `SUB`, `RSB`, `AND`, `ORR`, `EOR`, `BIC`, `LSL`, `LSR`, `ASR`, `ROR`, `CMP`, `MOV`, `MOVW`, `MOVT` (`max_alu = 2` concurrent issues per cycle).
  - `Port 2 (Complex Int FU / MUL)`: Handles `MUL`, `MLA`, `DIV` (MUL latency = 3 cycles, DIV latency = 12 cycles).
  - `Port 3 (Branch Unit)`: Handles conditional/unconditional branches and calls (`max_branch = 1`).
  - `Port 4 & Port 5 (LSU Load/Store)`: Handles loads (`max_load = 1`) and store address/data (`max_store = 1`).
  - Total Issue Width: 4-wide dispatch.

### 2.2 Instruction Sequence & Port Dispatch Analysis

#### `test_arithmetic.elf` Loop Body:
```assembly
arith_loop:
    movs r1, #20       ; ALU Port 0/1 (P0/P1)
    adds r1, r1, r6    ; ALU (dependent on r1)
    movs r2, #22       ; ALU (independent -> can co-issue on P1)
    adds r0, r1, r2    ; ALU (dependent on r1, r2)
    subs r0, r0, #10   ; ALU (dependent on r0)
    movs r1, #0xF0     ; ALU (independent)
    movs r2, #0x0F     ; ALU (independent)
    orrs r3, r1, r2    ; ALU
    ands r4, r1, r2    ; ALU (independent from ORR -> dual issue P0 & P1)
    eors r5, r1, r2    ; ALU (independent)
    movs r1, #1        ; ALU
    lsls r1, r1, #4    ; ALU
    muls r1, r6, r1    ; Complex Int MUL -> Port 2 (3 cycles latency)
    adds r7, r7, r0    ; ALU
    adds r7, r7, r3    ; ALU
    adds r7, r7, r5    ; ALU
    adds r7, r7, r1    ; ALU (stalled waiting for MUL to write back on Port 2)
    subs r6, r6, #1    ; ALU
    bne  arith_loop    ; Branch Port
```

**Port Contention Dynamics in `test_arithmetic.elf`**:
1. **ALU Port Saturation**: The loop has 17 ALU operations, 1 MUL operation, and 1 Branch. Because TinyCpuSim limits ALU issue to `max_alu = 2` per cycle, issuing 17 ALU ops requires a theoretical minimum of `ceil(17 / 2) = 9 cycles` per loop iteration under ideal dependency conditions.
2. **MUL Latency Stall**: `muls r1, r6, r1` takes 3 cycles to complete in Port 2 before `adds r7, r7, r1` can wake up in the reservation station (RS).
3. **Dispatch & RS Occupancy**: Over 200 iterations (19 insts/iter = 3,800 loop insts), 2,508 cycles are spent. Average IPC of **1.518** reflects sustained saturation of the dual ALU ports (`2.0 * (17/19) ≈ 1.79` theoretical ceiling minus MUL data hazard latency).

#### `test_raw_hazard.elf` Loop Body:
```assembly
raw_loop:
    adds r0, r0, #1    ; Cycle T: Issue P0 -> writes PRF(r0_v1) at T
    adds r0, r0, #2    ; Cycle T+1: Wakeup on PRF(r0_v1) -> Issue P0/P1 -> writes PRF(r0_v2)
    adds r0, r0, #3    ; Cycle T+2: Wakeup on PRF(r0_v2) -> Issue P0/P1 -> writes PRF(r0_v3)
    adds r0, r0, #4    ; Cycle T+3: Wakeup on PRF(r0_v3) -> Issue P0/P1 -> writes PRF(r0_v4)
    subs r1, r1, #1    ; Independent loop counter -> Can execute out-of-order in parallel!
    bne  raw_loop      ; Branch resolved
```

**Port Contention Dynamics in `test_raw_hazard.elf`**:
1. **Zero Port Contention, Total Dependency Bound**: Although 2 ALU ports and 4 issue slots are available, 4 out of 6 instructions in every iteration form a strict single-cycle RAW dependency chain (`r0 -> r0 -> r0 -> r0`).
2. **True Data Dependency Limit**: The dependency chain forces 1 instruction per cycle execution regardless of port width.
3. **Out-of-Order Overlap**: `subs r1, r1, #1` and `bne raw_loop` execute concurrently alongside the `r0` accumulate chain, achieving **IPC = 0.733** (6 instructions retired every ~8.2 cycles per iteration).

---

## 3. Top-Down Microarchitectural Breakdown

### 3.1 Comparison Table

```
================================================================================
                    Top-Down Level 1 Breakdown Comparison
================================================================================
  Metric Category             test_arithmetic.elf        test_raw_hazard.elf
--------------------------------------------------------------------------------
  Total Pipeline Slots               6,169                      6,922
  Retiring Slots (Effective Ops)     3,807 ( 61.71% )           3,008 ( 43.46% )
  Bad Speculation (Squashed)             0 (  0.00% )               0 (  0.00% )
  Front-End Bound                        0 (  0.00% )               0 (  0.00% )
  Back-End Bound                     2,362 ( 38.29% )           3,914 ( 56.54% )
    ├── Core Bound (RS/ROB Full)     2,362 ( 38.29% )           3,914 ( 56.54% )
    └── Memory Bound (L1D/MSHR)          0 (  0.00% )               0 (  0.00% )
================================================================================
```

### 3.2 Root Cause Analysis of Back-End Stalls:
- In `test_arithmetic.elf`, **38.29%** Backend Core Bound stalls occur because the 4-wide dispatch pipeline fills the 32-entry Issue Queue (RS) faster than the 2 ALU ports can drain ready instructions.
- In `test_raw_hazard.elf`, **56.54%** Backend Core Bound stalls occur because instructions remain unissued in the Reservation Station (RS) waiting for operand tags to wake up across the serialization chain.

---

## 4. Performance Slicing Analysis (`--slice-insts 1000`)

### 4.1 `test_arithmetic.elf` Slices

| Slice ID | Inst Interval | Tick Interval | Elapsed Cycles | Slice IPC | Retiring % | Backend Bound % |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **#0** | 0 -> 1,002 | 0 -> 735 | 735 | 1.363 | 61.89% | 38.11% |
| **#1** | 1,002 -> 2,002 | 735 -> 1,366 | 631 | 1.585 | 61.64% | 38.36% |
| **#2** | 2,002 -> 3,002 | 1,366 -> 1,997 | 631 | 1.585 | 61.64% | 38.36% |
| **#3** | 3,002 -> 3,807 | 1,997 -> 2,508 | 511 | 1.575 | 61.68% | 38.32% |

- **Phase Characteristics**: Slice #0 captures the branch predictor warmup and pipeline fill phase (IPC = 1.363). Slices #1 and #2 demonstrate steady-state loop execution with perfectly stable throughput (**IPC = 1.585**) and identical slot breakdowns.

### 4.2 `test_raw_hazard.elf` Slices

| Slice ID | Inst Interval | Tick Interval | Elapsed Cycles | Slice IPC | Retiring % | Backend Bound % |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **#0** | 0 -> 1,000 | 0 -> 1,432 | 1,432 | 0.698 | 43.33% | 56.67% |
| **#1** | 1,000 -> 2,000 | 1,432 -> 2,764 | 1,332 | 0.751 | 43.53% | 56.47% |
| **#2** | 2,000 -> 3,000 | 2,764 -> 4,096 | 1,332 | 0.751 | 43.53% | 56.47% |
| **#3** | 3,000 -> 3,008 | 4,096 -> 4,104 | 8 | 1.000 | 45.45% | 54.55% |

- **Phase Characteristics**: Slice #0 displays the startup penalty (IPC = 0.698). Slices #1 and #2 lock into a steady dependency cycle of exactly 1,332 cycles per 1,000 instructions (**IPC = 0.751**).

---

## 5. Microarchitectural Calibration Recommendations

1. **Port Dispatch Scalability**:
   - For integer-heavy workloads with high Instruction-Level Parallelism (ILP), expanding the integer execution ports from 2 Simple ALUs to 3 or 4 ALUs would reduce the IPC gap between TinyCpuSim (1.518) and gem5 (2.162) on `test_arithmetic.elf`.
2. **Issue Selection Priority**:
   - Oldest-first issue selection correctly prioritizes resolving long RAW dependency chains in `test_raw_hazard.elf`, accurately matching gem5 execution cycle counts within `<5.1%`.
3. **Verification Invariant**:
   - Zero memory order violations, 0 L1D cache thrashing, and >99% branch prediction accuracy across both arithmetic suites.
