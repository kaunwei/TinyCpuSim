# TinyCpuSim

[![Build & Test](https://img.shields.io/badge/tests-168%2F168%20passed-brightgreen.svg)]()
[![Standard](https://img.shields.io/badge/C%2B%2B-17-blue.svg)]()
[![ISA](https://img.shields.io/badge/ISA-ARMv7--M%20%2F%20Thumb--2-orange.svg)]()
[![License](https://img.shields.io/badge/license-MIT-green.svg)]()

**TinyCpuSim** is a high-performance, cycle-accurate Out-of-Order (OoO) superscalar CPU simulator and functional ISA emulator for ARMv7-M (Thumb-2) written in modern C++17.

TinyCpuSim provides an end-to-end microarchitectural exploration platform featuring an advanced dynamic Out-of-Order (RS/PRF/ROB) superscalar execution engine, non-blocking multi-level cache hierarchy with MESI multi-core coherence, Intel/ARM Top-Down microarchitecture analysis (TMAM), 40+ cycle-accurate hardware performance counters, dynamic Region of Interest (ROI) profiling via `m5ops`, and automated golden accuracy calibration against **gem5**.

---

## Table of Contents

- [Key Architectural Features](#key-architectural-features)
- [System Requirements & Prerequisites](#system-requirements--prerequisites)
- [Quickstart & Unified Interface (`run.sh`)](#quickstart--unified-interface-runsh)
- [Configuration Lifecycle Management (`config.py`)](#configuration-lifecycle-management-configpy)
- [Microarchitectural Experiment Engine (`experiment.py`)](#microarchitectural-experiment-engine-experimentpy)
- [Sequential 5-Step Workflow Guide](#sequential-5-step-workflow-guide)
  - [Step 1: Building Simulator & Tests (`01_build.sh`)](#step-1-building-simulator--tests-01_buildsh)
  - [Step 2: Full Unit & Regression Tests (`02_run_tests.sh`)](#step-2-full-unit--regression-tests-02_run_testssh)
  - [Step 3: Component Microbenchmarks (`03_run_ubench.sh`)](#step-3-component-microbenchmarks-03_run_ubenchsh)
  - [Step 4: Full CPU Simulation (`04_run_simulation.sh`)](#step-4-full-cpu-simulation-04_run_simulationsh)
  - [Step 5: gem5 Golden Reference Accuracy Comparison (`05_compare_gem5.sh`)](#step-5-gem5-golden-reference-accuracy-comparison-05_compare_gem5sh)
- [Hardware Performance Counters & TMAM Breakdown](#hardware-performance-counters--tmam-breakdown)
- [Project Directory Structure](#project-directory-structure)
- [License](#license)

---

## Key Architectural Features

- **ARMv7-M / Thumb-2 ISA Engine**:
  - Full Thumb-16 and Thumb-32 instruction decoding and emulation.
  - Complete arithmetic, logical, multiply/divide, load/store, branch, conditional execution (IT blocks), and stack operations.
  - Bare-metal ELF loader with memory space mapping and register initialization.

- **Superscalar Out-of-Order Core**:
  - **Branch Prediction Unit (BPU)**: Tournament Predictor, TAGE, GShare, Bimodal, Return Address Stack (RAS), and Branch Target Buffer (BTB).
  - **Frontend & Renaming**: Configurable N-wide superscalar Fetch/Decode/Rename with Physical Register File (PRF), Free Lists, and Speculative RAT checkpoints.
  - **Age-Ordered Unified Issue Queue / Reservation Station (RS)**: Tag-broadcast wakeup and priority-based scheduling (oldest instruction first) resolving structural and data hazards.
  - **Dual Execution Engine**: Dynamic Out-of-Order scheduling (`OOO_DYNAMIC`) or strict In-Order serialized stall execution (`enable_ooo = false`).
  - **Pipelined Execution Units**: Pipelined ALUs, Multipliers, Dividers, and Branch Resolution Units.
  - **Load/Store Unit (LSU)**: Load-Store Queue (LSQ) supporting store-to-load forwarding, speculative load bypass, and memory order violation detection.
  - **Reorder Buffer (ROB)**: In-order retirement supporting precise exception recovery and macro-instruction retirement tracking.

- **Non-blocking Memory & Cache Hierarchy**:
  - L1 Instruction Cache & L1 Data Cache with Miss Status Holding Registers (MSHRs).
  - Shared L2 Cache with LRU replacement policy and dirty write-back eviction.
  - MESI Cache Coherence state machine for multi-core simulation.

- **40+ Hardware Performance Counters & TMAM**:
  - Full execution port utilization, stall cycles, branch diagnostics, LSU forwarding rates, and cache/MSHR counters.
  - Cycle-accurate Top-Down slot accounting (Level 1 & Level 2 breakdown).
  - Dynamic Region of Interest (ROI) statistics collection via `m5_reset_stats()` and `m5_dump_stats()`.

---

## System Requirements & Prerequisites

TinyCpuSim has minimal external dependencies and runs on macOS and Linux.

- **Operating System**: macOS (Apple Silicon / Intel) or Linux (Ubuntu 20.04+, Debian, Fedora, Arch).
- **C++ Compiler**: Clang (`clang++ >= 11` / Apple Clang 13+) or GCC (`g++ >= 9`) with C++17 support.
- **Build System**: CMake `>= 3.15` and Make or Ninja.
- **Cross Compiler**: `arm-none-eabi-gcc` (for building Thumb-2 assembly test fixtures).
- **Python**: Python 3.8+ (with `keystone-engine`, `pyelftools`, `capstone`).

### Automated Dependency Installation

Install all required build tools and Python packages with one command:
```bash
./run.sh setup
# or directly: ./scripts/install_deps.sh
```

> **gem5 Oracle Dependency Notice (Optional)**:  
> **gem5 is strictly OPTIONAL**. TinyCpuSim embeds pre-cached empirical golden baselines in `tests/golden/gem5/` and `tests/uarch/golden_counters.json`. gem5 is **NOT** required for building the simulator, running unit tests, executing component ubenchmarks, or performing microarchitecture sweeps. For instructions on installing and compiling gem5 ARM from source, see [gem5 Setup & Calibration Guide](docs/gem5_setup.md).

---

## Quickstart & Unified Interface (`run.sh`)

TinyCpuSim provides a single unified entry point: [`run.sh`](run.sh).

### Interactive Menu

Launch the interactive text user interface:
```bash
./run.sh
```

```text
============================================================
              TinyCpuSim - Master Control Center            
============================================================
Please choose a step or action:
  [C] Config:   Configure Active Simulation, Hardware Knobs, Presets, Save/Load
  [1] Step 1:   Build Project (Release Mode)
  [2] Step 2:   Run Full Test Suite (168 Unit & Regression Tests)
  [3] Step 3:   Run Component Microbenchmarks (uBench)
  [4] Step 4:   Run CPU Simulation (reads configs/current.cfg automatically)
  [5] Step 5:   Compare Accuracy against gem5 Golden Reference
  [6] Exp:      Run Experiment on Active Config vs Baseline
  [7] Sweep:    Run Dynamic Parameter Sweep on Microarchitecture Knobs
  [H] Help:     View Complete Command & Usage Manual
  [0] Exit
============================================================
```

### Direct CLI Commands

```bash
# Workflow Steps
./run.sh build                      # [Step 1] Build simulator (Release mode)
./run.sh test                       # [Step 2] Run 168 unit & regression tests (parallel ctest)
./run.sh ubench [bpu|exec|rob|cache|all] # [Step 3] Run component microbenchmarks
./run.sh sim [elf] [config]         # [Step 4] Run simulation (zero-args reads current.cfg)
./run.sh gem5 [--all]               # [Step 5] Compare accuracy vs gem5 golden (parallel)

# Configuration & Editing
./run.sh edit                       # Direct vi editing of active configs/current.cfg
./run.sh show                       # Display full active microarchitecture dashboard
./run.sh list                       # List all presets and snapshots in default/ and save/
./run.sh config                     # Launch interactive configuration manager TUI

# Microarchitectural Experiments & Sweeps
./run.sh exp                        # Run experiment on active config (current.cfg) vs baseline
./run.sh exp [elf]                  # Run experiment on target ELF using active config
./run.sh exp --set k=v              # Run experiment with hardware overrides (e.g. ooo=false)
./run.sh sweep                      # Run dynamic parameter sweep across microarchitectural knobs

# Catalogs & Utilities
./run.sh knobs                      # List all tunable hardware parameters & units
./run.sh elfs                       # List all built-in benchmark ELF workloads
./run.sh clean                      # Clean build artifacts
./run.sh help                       # View complete CLI manual
```

---

### Configuration Lifecycle Management (`config.py`)

Configuration management is organized into a clean repository model under `configs/`:

```text
configs/
├── current.cfg             # Active simulation configuration (linked/copied from default/)
├── all_params_template.cfg # Comprehensive template documenting all knobs
├── default/                # Canonical baseline presets
│   ├── default.cfg             # 4-wide OoO (Dynamic RS/PRF/ROB), TAGE BPU, 32KB L1I/D, 512KB L2
│   ├── multicore_4core.cfg     # 4 Cores OoO, MESI Cache Coherence, 2MB Shared L2
│   ├── mem_fast_feeder.cfg     # FAST_FEEDER core + IDEAL BPU + full cache hierarchy (50x fast mem eval)
│   ├── bpu_standalone.cfg      # FAST_FEEDER core + TAGE BPU + ZERO_LATENCY caches (isolated BPU eval)
│   └── inorder_embedded.cfg    # SIMPLE_INORDER 1-wide core + BIMODAL + un-cached passthrough
└── save/                   # User custom snapshots (e.g. my_opt_v1.cfg)
```

### Polymorphic Subsystem Selection

Instead of monolithic binary switches, every subsystem supports pluggable polymorphic module types:

| Subsystem | Configuration Key | Supported Polymorphic Options | Purpose / Behavior |
| :--- | :--- | :--- | :--- |
| **Execution Core** | `core.type` | `OOO_DYNAMIC`, `SIMPLE_INORDER`, `FAST_FEEDER` | Full out-of-order dynamic RS/PRF/ROB execution, strict sequential in-order issue, or high-throughput fast feeder. |
| **Branch Predictor** | `branch_predictor.type` | `NONE`, `IDEAL`, `BIMODAL`, `GSHARE`, `TAGE` | Hardware branch predictor algorithms or ideal 100% bypass. |
| **Load/Store Unit** | `lsu.type` | `SPECULATIVE_OOO`, `STRICT_INORDER`, `PASSTHROUGH` | Speculative store forwarding / memory disambiguation, or passthrough. |
| **Cache Hierarchy** | `cache.type` | `SET_ASSOCIATIVE`, `DIRECT_MAPPED`, `ZERO_LATENCY`, `PASSTHROUGH` | Multi-way set associative, direct mapped, zero-latency hit, or un-cached bypass. |
| **Coherence** | `system.coherence` | `MESI`, `NONE` | Multi-core MESI snooping protocol or disabled. |

### Key Capabilities:
- **Direct Editor Integration**: Edit the active configuration directly using `vi` via `./run.sh edit`.
- **Human-Readable Unit Conversion**: Write cache sizes as `32KB`, `512KB`, or `1MB`. Values are validated and automatically converted to power-of-2 integer bytes upon saving while preserving all comments.
- **Microarchitecture Dashboard**: Run `./run.sh show` to inspect all 6 hardware subsystems:
  1. Target Workload
  2. Multicore / MESI Coherence / DRAM Latency
  3. Superscalar Pipeline & Execution Engine (`OOO_DYNAMIC`, `SIMPLE_INORDER`, `FAST_FEEDER`)
  4. Branch Prediction Unit (TAGE / BTB / RAS / Ideal)
  5. Load/Store Unit & Memory Disambiguation (`SPECULATIVE_OOO`, `PASSTHROUGH`)
  6. Multi-Level Cache Hierarchy (L1I, L1D, Shared L2, MSHRs)

---

## Microarchitectural Experiment Engine (`experiment.py`)

TinyCpuSim features a zero-prompt experiment comparison engine (`./run.sh exp`).

### How It Works:
1. **Decodes Target ELF**: Resolves the workload from CLI arguments (e.g. `./run.sh exp test_sort.elf`) or directly from `[workload] elf_path` inside `configs/current.cfg`.
2. **Auto-Resolves Baseline**: Checks `reports/default/<elf>.txt` for a cached baseline report.
3. **Deltas & Diagnosis**: Compares the active configuration against the baseline, producing a 5-panel structured comparison report with an automated architectural root-cause diagnosis.
4. **Missing Baseline Handling**: If no baseline report is found, it runs the simulation, prints the full performance report, and outputs the exact command to establish it as baseline:
   ```bash
   cp reports/exp_<elf>_<timestamp>.txt reports/default/<elf>.txt
   ```

### Example Comparison Report:
```text
====================================================================================
       TinyCpuSim Microarchitectural Experiment Report: [test_fibonacci.elf]
====================================================================================
  Target Program:        /Users/.../tests/fixtures/test_fibonacci.elf
  Baseline Report:       /Users/.../reports/default/test_fibonacci.txt
  Applied Configuration: /Users/.../configs/current.cfg
------------------------------------------------------------------------------------
  Active Hardware Parameter Modifications (vs Baseline):
    • [core] issue_width: 4  -->  2
------------------------------------------------------------------------------------
  Executive Impact: SLOWDOWN: 1.21x (17.5% slower) | Baseline IPC: 0.801 -> Exp IPC: 0.661
------------------------------------------------------------------------------------
  1. Overall Core Performance        | Baseline      | Experiment    | Delta (%)   
  --------------------------------------------------------------------------------
  Simulated Total Cycles             | 2132          | 2584          | +21.20%
  Throughput (IPC)                   | 0.80          | 0.66          | -17.48%
  uOp Throughput (uOp IPC)           | 1.55          | 1.28          | -17.51%
  Committed Instructions             | 1708          | 1708          | 0.0%        
  Committed uOps                     | 3301          | 3301          | 0.0%        

  --------------------------------------------------------------------------------
  2. Top-Down TMAM Breakdown         | Baseline      | Experiment    | Delta (%)   
  --------------------------------------------------------------------------------
  Retiring (Useful Work)             | 100.00%       | 90.39%        | -9.61%
  Bad Speculation (Squashed)         | 0.00%         | 0.00%         | ---         
  Front-End Bound (Fetch/BTB)        | 0.00%         | 0.00%         | ---         
  Back-End Bound (Stalls)            | 0.00%         | 9.61%         | +9.61

  --------------------------------------------------------------------------------
  3. Pipeline Stalls & Hazards       | Baseline      | Experiment    | Delta (%)   
  --------------------------------------------------------------------------------
  Issue Queue (RS) Full Stalls       | 0 cyc         | 351 cyc       | +351
  ROB Full Stalls                    | 0 cyc         | 0 cyc         | ---         
  PRF FreeList Exhaustion Stalls     | 0 cyc         | 0 cyc         | ---         
  LQ / SQ Full Stalls                | 0 cyc         | 0 cyc         | ---         
  Head-of-ROB Stalls                 | 0 cyc         | 0 cyc         | ---         

  --------------------------------------------------------------------------------
  4. Branch Predictor & Control      | Baseline      | Experiment    | Delta (%)   
  --------------------------------------------------------------------------------
  Branch Predictor Accuracy          | 74.20%        | 74.90%        | +0.94%
  Branch Mispredict Penalty Flushes  | 235           | 210           | -10.64%
  BTB Target Hit Rate                | 0.00%         | 0.00%         | ---         
  RAS Return Hit Rate                | 0.00%         | 0.00%         | ---         

  --------------------------------------------------------------------------------
  5. Memory & Cache Subsystem        | Baseline      | Experiment    | Delta (%)   
  --------------------------------------------------------------------------------
  L1I Cache Hit Rate                 | 99.96%        | 99.97%        | +0.01%
  L1D Cache Hit Rate                 | 99.64%        | 99.62%        | -0.02%
  Shared L2 Cache Hit Rate           | 0.00%         | 0.00%         | ---         
  Store-to-Load Bypass Rate          | 8.52%         | 0.00%         | -100.00%
  L1D MSHR Saturation Stalls         | 0             | 0             | ---         

====================================================================================
  🔍 Automated Architectural Diagnosis & Key Takeaways:
  • Performance Degradation: Execution took 21.2% more cycles, IPC dropped by -17.5%.
  • Issue Queue Bottleneck: Issue Queue / RS stalls increased by +351 cycles.
  • Branch Improvement: Eliminated 25 branch mispredict flushes.
====================================================================================
  Experiment performance report saved to: reports/exp_test_fibonacci_20260926_233010.txt
```

---

## Sequential 5-Step Workflow Guide

### Step 1: Building Simulator & Tests (`01_build.sh`)
```bash
./scripts/01_build.sh
```
Automatically detects system CPU cores, configures CMake in Release mode, and builds `build/tinycpusim` alongside all test binaries.

---

### Step 2: Full Unit & Regression Tests (`02_run_tests.sh`)
```bash
./scripts/02_run_tests.sh
```
Runs all 168 unit and regression tests in parallel (`ctest -j`) with a 100% pass guarantee.

---

### Step 3: Component Microbenchmarks & Performance Suite (`03_run_ubench.sh` / `run_ubench.py`)
```bash
# Launch interactive microbenchmark suite selector
./run.sh ubench

# Run specific component suites directly
./run.sh ubench all     # Run all 41 microbenchmarks across full CPU
./run.sh ubench bpu     # Branch Predictor & Frontend (TAGE, BTB, RAS, Fetch, PRF)
./run.sh ubench exec    # Execution Engine & LSU (Issue width, Forwarding, Replay)
./run.sh ubench rob     # Reorder Buffer & Top-Down (Retirement, Head blocking, TMAM)
./run.sh ubench cache   # Non-blocking Caches (L1I/L1D hit latency, MSHRs, MESI)
```

#### Key Capabilities:
- **Parallel Execution**: Multi-process worker pool (auto-configured to half available CPU cores or `-j/--jobs`) speeds up batch runs.
- **Baseline Tracking (`reports/ubench/default/`)**: Automatically compares active runs against baseline statistics with cycle deltas.
- **Top Performers vs Bottlenecks**: Automatically ranks and isolates optimal subsystem efficiencies vs critical performance bottlenecks.
- **Detailed Report Output (`reports/ubench/`)**: Saves full microarchitectural logs for every run into `reports/ubench/`.

---

### Step 4: Full CPU Simulation (`04_run_simulation.sh`)
```bash
# Run simulation using active configs/current.cfg
./scripts/04_run_simulation.sh

# Run specific ELF workload
./scripts/04_run_simulation.sh tests/fixtures/test_fibonacci.elf
```

---

### Step 5: gem5 Golden Reference Accuracy Comparison (`05_compare_gem5.sh`)
```bash
# Run batch accuracy regression across all benchmarks
./scripts/05_compare_gem5.sh --all
```

---

## Hardware Performance Counters & TMAM Breakdown

Simulating with `--all-perf` outputs complete hardware statistics:

```text
============================================================
               TinyCpuSim uArch Simulation Report           
============================================================
Simulated Target Clock:    1000.00 MHz
Simulated Total Cycles:    2132 ticks
Total Committed Insts:     1708
Total Committed uOps:      3301
Aggregate Throughput (IPC):0.801 inst/cycle (uOp IPC: 1.548)
------------------------------------------------------------
[ Core 0 Summary ]
  Committed Insts / uOps:  1708 / 3301 (uOp Ratio: 1.93x)
  Core Throughput (IPC):   0.801 inst/cycle
  Branch Predictions:      911 (Accuracy: 74.2%)
  Branch Mispredicts:      235 (Penalty Flushes: 235)
  L1I Cache Hit Rate:      99.96% (5608/5610)
  L1D Cache Hit Rate:      99.64% (1107/1111)
  Loads / Stores:          634 / 531
  Store-to-Load Forwards:  54 (Rate: 8.52%)
  Mem Order Violations:    0
  --- Top-Down Breakdown (Level 1) ---
    Frontend Bound:        18.42%
    Bad Speculation:       4.12%
    Backend Bound:         42.85%
    Retiring:              34.61%
  --- Detailed Execution Ports ---
    Port ALU uOps:         1465
    Port MUL uOps:         0
    Port DIV uOps:         0
    Port Branch uOps:      700
    Port LSU uOps:         1815
  --- Pipeline Stalls Breakdown ---
    ROB Full Stalls:       0 cycles
    RS/IQ Full Stalls:     0 cycles
    PRF Exhaustion Stalls: 0 cycles
    LQ / SQ Full Stalls:   0 / 0 cycles
    Head-of-ROB Stalls:    0 cycles
  --- Branch Predictor Diagnostics ---
    Direct Cond / Uncond:  321 / 0
    Calls / Returns:       379 / 0
    Indirect Branches:     0
    BTB Hits / Misses:     589 / 322 (Hit Rate: 64.6%)
    RAS Hits / Misses:     379 / 0 (Hit Rate: 100.0%)
  --- LSU & Memory Disambiguation ---
    Store-to-Load Forwards:54
    Store Data Replays:    0
    Memory Order Flushes:  0
  --- Cache & MSHR Subsystem ---
    L1I Misses / Evictions:2 / 0
    L1D Misses / Evictions:4 / 0
    L1D Dirty Writebacks:  0
    L1D MSHR Allocations:  0 (Stalls: 0)
============================================================
```

---

## Project Directory Structure

```text
TinySim/
├── CMakeLists.txt            # Main CMake build configuration
├── README.md                 # Complete documentation & usage guide
├── run.sh                    # Unified launcher & workflow manager
├── configs/                  # Microarchitecture configurations
│   ├── current.cfg           # Active simulation configuration
│   ├── all_params_template.cfg # Template of all configuration knobs
│   ├── default/              # Baseline presets (default.cfg, multicore_4core.cfg, etc.)
│   └── save/                 # Custom saved snapshot configs
├── include/tinyarmsim/       # Public C++ headers
│   ├── isa/                  # ISA decoder, instruction definitions, register state
│   ├── memory/               # Memory bus, non-blocking caches, MESI coherence
│   └── uarch/                # OoO pipeline: BPU, Frontend, PRF, Issue, Exec, LSU, ROB, TMAM
├── src/                      # Source implementations
│   ├── main.cpp              # CLI driver with --uarch, --all-perf, and options
│   ├── isa/                  # ISA simulation & disassembler
│   ├── memory/               # Memory bus & cache controller
│   └── uarch/                # Cycle-accurate OoO pipeline stages
├── scripts/                  # Workflow scripts & multi-process Python tools
│   ├── 01_build.sh           # Step 1: 1-click build script
│   ├── 02_run_tests.sh       # Step 2: Full test suite runner (168 tests, parallel ctest)
│   ├── 03_run_ubench.sh      # Step 3: Component microbenchmark runner
│   ├── 04_run_simulation.sh  # Step 4: Full-system simulation runner
│   ├── 05_compare_gem5.sh    # Step 5: gem5 golden comparison tool
│   ├── config.py             # Configuration lifecycle manager & TUI
│   ├── experiment.py         # Microarchitectural experiment engine & comparison
│   ├── sweep_parameters.py   # Multi-process parameter sweep engine
│   ├── run_ubench.py         # Multi-process uBench suite runner
│   ├── report_ubench_perf.py # uBench baseline & performance validator
│   └── compare_with_gem5.py  # Multi-process gem5 accuracy comparator
├── reports/                  # Generated experiment & simulation reports
│   ├── default/              # Cached default baseline reports for test ELFs
│   └── ubench/               # Component microbenchmark baseline & run reports
└── tests/                    # Tests and benchmarks
    ├── fixtures/             # Bare-metal ELF binaries & assembly sources
    ├── golden/gem5/          # gem5 reference statistics logs
    └── unit & ubench tests   # 168 CTest GoogleTest cases
```

---

## License

This project is licensed under the MIT License - see the LICENSE file for details.
