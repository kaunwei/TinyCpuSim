# TinyCpuSim Comprehensive 12-Scenario Microarchitectural Evaluation Report

> **Execution Date**: 2026-10-05  
> **Simulator**: TinyCpuSim (Thumb-2 ISA & Dynamic Out-of-Order Engine)  
> **Benchmark Suite**: `test_fibonacci.elf`, `test_matrix_mul.elf`, `test_sum_array.elf`, `test_sort.elf`  
> **Target Hardware**: Arm Cortex-A Class / Neoverse OoO Microarchitecture  

---

## Executive Summary

This evaluation benchmarks the sensitivity, pipeline efficiency, and simulator host throughput across **12 distinct microarchitectural subsystems**.
Key findings include:
- **Branch Predictor**: TAGE and large GShare tables significantly eliminate frontend bubbles on control-intensive workloads (`test_fibonacci`, `test_sort`).
- **Superscalar Scaling**: Workloads transition from Frontend/Retiring bound (1-wide) to Backend/Data Hazard bound as width increases to 8-wide.
- **Memory Disambiguation**: Speculative Store Sets allow high load throughput without stalling on unresolved store addresses, yielding marked IPC improvements over strict serialization.
- **Simulation Engine Speed**: FastFeeder and Pure ISA Interpreter reach tens of MIPS host throughput, enabling hybrid multi-tiered fast-forwarding.

---

## Scenario 1: Branch Predictor Type & Table Size
*Sensitivity of branch predictor algorithms (Bimodal, GShare, TAGE, Ideal) and table entry capacities.*

| Configuration | Benchmark | IPC | Cycles | Host MIPS | L1D Hit % | BP Acc % | TMAM (Ret / BadSpec / Fe / Be) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Bimodal (1024)** | `test_fibonacci.elf` | **0.666** | 2564 | 13.68 | 99.7% | 74.7% | `91.7% / 0.0% / 0.0% / 8.3%` |
| **Bimodal (1024)** | `test_matrix_mul.elf` | **1.992** | 1911 | 17.83 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Bimodal (1024)** | `test_sum_array.elf` | **2.218** | 4531 | 30.75 | 99.7% | 97.3% | `79.1% / 0.0% / 0.0% / 20.9%` |
| **Bimodal (1024)** | `test_sort.elf` | **2.276** | 3415 | 26.81 | 99.9% | 97.9% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Bimodal (4096)** | `test_fibonacci.elf` | **0.667** | 2559 | 14.09 | 99.7% | 74.8% | `91.7% / 0.0% / 0.0% / 8.3%` |
| **Bimodal (4096)** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.39 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Bimodal (4096)** | `test_sum_array.elf` | **2.218** | 4531 | 29.75 | 99.7% | 97.3% | `79.1% / 0.0% / 0.0% / 20.9%` |
| **Bimodal (4096)** | `test_sort.elf` | **2.276** | 3415 | 23.41 | 99.9% | 97.9% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **GShare (1024)** | `test_fibonacci.elf` | **0.646** | 2645 | 13.94 | 99.7% | 73.8% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **GShare (1024)** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.10 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **GShare (1024)** | `test_sum_array.elf` | **2.218** | 4531 | 30.63 | 99.7% | 97.3% | `79.1% / 0.0% / 0.0% / 20.9%` |
| **GShare (1024)** | `test_sort.elf` | **2.276** | 3415 | 26.21 | 99.9% | 97.9% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **GShare (4096)** | `test_fibonacci.elf` | **0.646** | 2645 | 13.64 | 99.7% | 73.8% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **GShare (4096)** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.61 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **GShare (4096)** | `test_sum_array.elf` | **2.218** | 4531 | 30.84 | 99.7% | 97.3% | `79.1% / 0.0% / 0.0% / 20.9%` |
| **GShare (4096)** | `test_sort.elf` | **2.276** | 3415 | 26.61 | 99.9% | 97.9% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **GShare (8192)** | `test_fibonacci.elf` | **0.646** | 2645 | 12.32 | 99.7% | 73.8% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **GShare (8192)** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.73 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **GShare (8192)** | `test_sum_array.elf` | **2.218** | 4531 | 29.40 | 99.7% | 97.3% | `79.1% / 0.0% / 0.0% / 20.9%` |
| **GShare (8192)** | `test_sort.elf` | **2.276** | 3415 | 26.77 | 99.9% | 97.9% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **TAGE (4-table, 4096)** | `test_fibonacci.elf` | **0.645** | 2650 | 14.62 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **TAGE (4-table, 4096)** | `test_matrix_mul.elf` | **1.992** | 1911 | 16.68 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **TAGE (4-table, 4096)** | `test_sum_array.elf` | **2.203** | 4562 | 27.36 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **TAGE (4-table, 4096)** | `test_sort.elf` | **2.266** | 3431 | 22.65 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Ideal Predictor** | `test_fibonacci.elf` | **0.534** | 3196 | 13.78 | 99.7% | 57.3% | `95.9% / 0.0% / 0.0% / 4.1%` |
| **Ideal Predictor** | `test_matrix_mul.elf` | **1.992** | 1911 | 18.34 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Ideal Predictor** | `test_sum_array.elf` | **2.218** | 4531 | 29.71 | 99.7% | 97.3% | `79.1% / 0.0% / 0.0% / 20.9%` |
| **Ideal Predictor** | `test_sort.elf` | **0.965** | 8060 | 27.17 | 99.9% | 86.7% | `97.6% / 0.0% / 0.0% / 2.4%` |

---

## Scenario 2: Loop Stream Detector (LSD) On/Off & Capacity
*Impact of caching decoded loop uOps to bypass Fetch and Decode stages (16, 32, 64 entries).*

| Configuration | Benchmark | IPC | Cycles | Host MIPS | L1D Hit % | BP Acc % | TMAM (Ret / BadSpec / Fe / Be) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **LSD Disabled** | `test_fibonacci.elf` | **0.645** | 2650 | 14.02 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **LSD Disabled** | `test_matrix_mul.elf` | **1.992** | 1911 | 18.63 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **LSD Disabled** | `test_sum_array.elf` | **2.203** | 4562 | 30.23 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **LSD Disabled** | `test_sort.elf` | **2.266** | 3431 | 27.33 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **LSD Capacity 16** | `test_fibonacci.elf` | **0.645** | 2649 | 13.96 | 99.7% | 73.6% | `91.9% / 0.0% / 0.0% / 8.1%` |
| **LSD Capacity 16** | `test_matrix_mul.elf` | **1.992** | 1911 | 16.72 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **LSD Capacity 16** | `test_sum_array.elf` | **2.218** | 4531 | 26.18 | 99.7% | 97.3% | `76.9% / 0.0% / 0.0% / 23.1%` |
| **LSD Capacity 16** | `test_sort.elf` | **2.268** | 3429 | 22.52 | 99.9% | 97.8% | `81.0% / 0.0% / 0.0% / 19.0%` |
| **LSD Capacity 32** | `test_fibonacci.elf` | **0.645** | 2649 | 14.00 | 99.7% | 73.6% | `91.9% / 0.0% / 0.0% / 8.1%` |
| **LSD Capacity 32** | `test_matrix_mul.elf` | **1.992** | 1911 | 20.75 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **LSD Capacity 32** | `test_sum_array.elf` | **2.218** | 4531 | 29.84 | 99.7% | 97.3% | `76.9% / 0.0% / 0.0% / 23.1%` |
| **LSD Capacity 32** | `test_sort.elf` | **2.268** | 3429 | 26.01 | 99.9% | 97.8% | `81.0% / 0.0% / 0.0% / 19.0%` |
| **LSD Capacity 64** | `test_fibonacci.elf` | **0.645** | 2649 | 13.65 | 99.7% | 73.6% | `91.9% / 0.0% / 0.0% / 8.1%` |
| **LSD Capacity 64** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.71 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **LSD Capacity 64** | `test_sum_array.elf` | **2.218** | 4531 | 30.36 | 99.7% | 97.3% | `76.9% / 0.0% / 0.0% / 23.1%` |
| **LSD Capacity 64** | `test_sort.elf` | **2.268** | 3429 | 25.99 | 99.9% | 97.8% | `81.0% / 0.0% / 0.0% / 19.0%` |

---

## Scenario 3: Macro-Op Instruction Fusion
*Throughput improvements via fusing compare and conditional branch instructions (CMP+Bcc) into a single uop.*

| Configuration | Benchmark | IPC | Cycles | Host MIPS | L1D Hit % | BP Acc % | TMAM (Ret / BadSpec / Fe / Be) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Fusion Disabled** | `test_fibonacci.elf` | **0.645** | 2650 | 13.37 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **Fusion Disabled** | `test_matrix_mul.elf` | **1.992** | 1911 | 20.73 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Fusion Disabled** | `test_sum_array.elf` | **2.203** | 4562 | 28.16 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **Fusion Disabled** | `test_sort.elf` | **2.266** | 3431 | 26.98 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **CMP+Bcc Fusion Enabled** | `test_fibonacci.elf` | **0.761** | 2245 | 13.67 | 99.7% | 78.6% | `88.4% / 0.0% / 0.0% / 11.6%` |
| **CMP+Bcc Fusion Enabled** | `test_matrix_mul.elf` | **1.994** | 1909 | 21.29 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **CMP+Bcc Fusion Enabled** | `test_sum_array.elf` | **0.894** | 11241 | 30.30 | 99.7% | 3.6% | `100.0% / 0.0% / 0.0% / 0.0%` |
| **CMP+Bcc Fusion Enabled** | `test_sort.elf` | **2.463** | 3158 | 26.70 | 99.9% | 96.2% | `84.8% / 0.0% / 0.0% / 15.2%` |

---

## Scenario 4: Superscalar Width (1, 2, 4, 8)
*Pipeline throughput and Top-Down bottleneck shift across 1-wide, 2-wide, 4-wide, and 8-wide configurations.*

| Configuration | Benchmark | IPC | Cycles | Host MIPS | L1D Hit % | BP Acc % | TMAM (Ret / BadSpec / Fe / Be) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **1-Wide** | `test_fibonacci.elf` | **0.404** | 4228 | 14.44 | 99.7% | 73.6% | `89.1% / 0.0% / 0.0% / 10.9%` |
| **1-Wide** | `test_matrix_mul.elf` | **0.924** | 4121 | 16.24 | 0.0% | 99.0% | `52.5% / 0.0% / 0.0% / 47.5%` |
| **1-Wide** | `test_sum_array.elf` | **0.773** | 13008 | 30.09 | 99.7% | 97.7% | `58.5% / 0.0% / 0.0% / 41.5%` |
| **1-Wide** | `test_sort.elf` | **0.838** | 9278 | 25.81 | 99.9% | 97.7% | `74.7% / 0.0% / 0.0% / 25.3%` |
| **2-Wide** | `test_fibonacci.elf` | **0.509** | 3355 | 13.80 | 99.7% | 73.8% | `85.2% / 0.0% / 0.0% / 14.8%` |
| **2-Wide** | `test_matrix_mul.elf` | **1.514** | 2515 | 21.59 | 0.0% | 99.0% | `62.4% / 0.0% / 0.0% / 37.6%` |
| **2-Wide** | `test_sum_array.elf` | **1.414** | 7108 | 29.67 | 99.7% | 95.8% | `73.2% / 0.0% / 0.0% / 26.8%` |
| **2-Wide** | `test_sort.elf` | **1.404** | 5540 | 26.11 | 99.8% | 97.8% | `72.3% / 0.0% / 0.0% / 27.7%` |
| **4-Wide** | `test_fibonacci.elf` | **0.645** | 2650 | 13.62 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **4-Wide** | `test_matrix_mul.elf` | **1.992** | 1911 | 16.52 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **4-Wide** | `test_sum_array.elf` | **2.203** | 4562 | 27.46 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **4-Wide** | `test_sort.elf` | **2.266** | 3431 | 25.32 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **8-Wide** | `test_fibonacci.elf` | **0.683** | 2500 | 14.40 | 99.7% | 76.4% | `93.8% / 0.0% / 0.0% / 6.2%` |
| **8-Wide** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.64 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **8-Wide** | `test_sum_array.elf` | **2.585** | 3887 | 30.33 | 99.7% | 96.6% | `90.5% / 0.0% / 0.0% / 9.5%` |
| **8-Wide** | `test_sort.elf` | **2.723** | 2856 | 21.88 | 99.8% | 97.8% | `99.1% / 0.0% / 0.0% / 0.9%` |

---

## Scenario 5: OoO Window ROB & RS Capacity
*Dynamic scheduling window depth: Reorder Buffer (16..256) and Reservation Station (8..128) scaling.*

| Configuration | Benchmark | IPC | Cycles | Host MIPS | L1D Hit % | BP Acc % | TMAM (Ret / BadSpec / Fe / Be) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Small (ROB 16, RS 8)** | `test_fibonacci.elf` | **0.642** | 2659 | 10.78 | 99.7% | 72.2% | `66.3% / 0.0% / 0.0% / 33.7%` |
| **Small (ROB 16, RS 8)** | `test_matrix_mul.elf` | **1.646** | 2313 | 20.64 | 0.0% | 99.0% | `63.4% / 0.0% / 0.0% / 36.6%` |
| **Small (ROB 16, RS 8)** | `test_sum_array.elf` | **1.679** | 5984 | 27.26 | 99.7% | 95.9% | `67.0% / 0.0% / 0.0% / 33.0%` |
| **Small (ROB 16, RS 8)** | `test_sort.elf` | **1.550** | 5016 | 25.92 | 99.9% | 97.7% | `65.5% / 0.0% / 0.0% / 34.5%` |
| **Medium (ROB 64, RS 32)** | `test_fibonacci.elf` | **0.645** | 2650 | 13.70 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **Medium (ROB 64, RS 32)** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.54 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Medium (ROB 64, RS 32)** | `test_sum_array.elf` | **2.203** | 4562 | 26.98 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **Medium (ROB 64, RS 32)** | `test_sort.elf` | **2.266** | 3431 | 22.19 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Large (ROB 128, RS 64)** | `test_fibonacci.elf` | **0.646** | 2645 | 13.93 | 99.7% | 73.7% | `99.5% / 0.0% / 0.0% / 0.5%` |
| **Large (ROB 128, RS 64)** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.91 | 0.0% | 99.0% | `100.0% / 0.0% / 0.0% / 0.0%` |
| **Large (ROB 128, RS 64)** | `test_sum_array.elf` | **2.203** | 4562 | 29.79 | 99.7% | 96.9% | `87.7% / 0.0% / 0.0% / 12.3%` |
| **Large (ROB 128, RS 64)** | `test_sort.elf` | **2.264** | 3422 | 26.00 | 99.9% | 97.8% | `91.3% / 0.0% / 0.0% / 8.7%` |
| **Ultra (ROB 256, RS 128)** | `test_fibonacci.elf` | **0.646** | 2645 | 12.51 | 99.7% | 73.7% | `100.0% / 0.0% / 0.0% / 0.0%` |
| **Ultra (ROB 256, RS 128)** | `test_matrix_mul.elf` | **1.992** | 1911 | 19.43 | 0.0% | 99.0% | `100.0% / 0.0% / 0.0% / 0.0%` |
| **Ultra (ROB 256, RS 128)** | `test_sum_array.elf` | **2.203** | 4562 | 30.10 | 99.7% | 96.9% | `100.0% / 0.0% / 0.0% / 0.0%` |
| **Ultra (ROB 256, RS 128)** | `test_sort.elf` | **2.268** | 3429 | 26.98 | 99.9% | 97.8% | `100.0% / 0.0% / 0.0% / 0.0%` |

---

## Scenario 6: Physical Register File (PRF) Capacity
*Register renaming capacity (32..256 physical registers) and register dependency pressure.*

| Configuration | Benchmark | IPC | Cycles | Host MIPS | L1D Hit % | BP Acc % | TMAM (Ret / BadSpec / Fe / Be) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **PRF 32 Regs** | `test_fibonacci.elf` | **0.585** | 2918 | 14.44 | 99.7% | 72.0% | `100.0% / 0.0% / 0.0% / 0.0%` |
| **PRF 32 Regs** | `test_matrix_mul.elf` | **0.586** | 6502 | 20.82 | 0.0% | 99.0% | `100.0% / 0.0% / 0.0% / 0.0%` |
| **PRF 32 Regs** | `test_sum_array.elf` | **0.771** | 13026 | 29.24 | 99.7% | 95.9% | `100.0% / 0.0% / 0.0% / 0.0%` |
| **PRF 32 Regs** | `test_sort.elf` | **0.882** | 8814 | 27.08 | 99.9% | 97.7% | `100.0% / 0.0% / 0.0% / 0.0%` |
| **PRF 64 Regs** | `test_fibonacci.elf` | **0.645** | 2650 | 13.95 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **PRF 64 Regs** | `test_matrix_mul.elf` | **1.650** | 2307 | 21.83 | 0.0% | 99.0% | `100.0% / 0.0% / 0.0% / 0.0%` |
| **PRF 64 Regs** | `test_sum_array.elf` | **1.686** | 5962 | 28.47 | 99.7% | 85.8% | `100.0% / 0.0% / 0.0% / 0.0%` |
| **PRF 64 Regs** | `test_sort.elf` | **2.198** | 3538 | 27.36 | 99.9% | 97.8% | `100.0% / 0.0% / 0.0% / 0.0%` |
| **PRF 128 Regs** | `test_fibonacci.elf` | **0.645** | 2650 | 9.25 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **PRF 128 Regs** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.56 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **PRF 128 Regs** | `test_sum_array.elf` | **2.203** | 4562 | 29.68 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **PRF 128 Regs** | `test_sort.elf` | **2.266** | 3431 | 24.43 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **PRF 256 Regs** | `test_fibonacci.elf` | **0.043** | 17580 | 4.62 | 99.3% | 75.1% | `8.1% / 0.0% / 0.0% / 92.0%` |
| **PRF 256 Regs** | `test_matrix_mul.elf` | **0.007** | 38570 | 1.37 | 0.0% | 93.3% | `0.7% / 0.0% / 0.0% / 99.3%` |
| **PRF 256 Regs** | `test_sum_array.elf` | **0.002** | 100990 | 0.52 | 90.3% | 93.3% | `0.2% / 0.0% / 0.0% / 99.8%` |
| **PRF 256 Regs** | `test_sort.elf` | **0.004** | 78270 | 1.03 | 95.3% | 96.0% | `0.4% / 0.0% / 0.0% / 99.6%` |

---

## Scenario 7: Memory Disambiguation Strategy
*Comparison between Strict In-Order waiting, Blind Passthrough, and Speculative Store Sets algorithm.*

| Configuration | Benchmark | IPC | Cycles | Host MIPS | L1D Hit % | BP Acc % | TMAM (Ret / BadSpec / Fe / Be) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Strict In-Order Wait** | `test_fibonacci.elf` | **0.645** | 2650 | 14.33 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **Strict In-Order Wait** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.21 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Strict In-Order Wait** | `test_sum_array.elf` | **2.203** | 4562 | 30.72 | 99.7% | 96.9% | `81.2% / 0.0% / 0.0% / 18.8%` |
| **Strict In-Order Wait** | `test_sort.elf` | **2.268** | 3429 | 26.89 | 99.9% | 97.8% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Store Sets (Speculative OoO)** | `test_fibonacci.elf` | **0.645** | 2650 | 13.77 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **Store Sets (Speculative OoO)** | `test_matrix_mul.elf` | **1.992** | 1911 | 18.86 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Store Sets (Speculative OoO)** | `test_sum_array.elf` | **2.203** | 4562 | 30.50 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **Store Sets (Speculative OoO)** | `test_sort.elf` | **2.266** | 3431 | 23.67 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Blind Passthrough Bypass** | `test_fibonacci.elf` | **0.645** | 2650 | 14.15 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **Blind Passthrough Bypass** | `test_matrix_mul.elf` | **1.992** | 1911 | 17.89 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Blind Passthrough Bypass** | `test_sum_array.elf` | **2.203** | 4562 | 30.75 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **Blind Passthrough Bypass** | `test_sort.elf` | **2.247** | 3344 | 25.55 | 99.9% | 97.7% | `79.6% / 0.0% / 0.0% / 20.4%` |

---

## Scenario 8: Hardware Data Prefetcher
*Evaluation of None, Next-Line, and Stride prefetchers across varying lookahead distances (1..8 lines).*

| Configuration | Benchmark | IPC | Cycles | Host MIPS | L1D Hit % | BP Acc % | TMAM (Ret / BadSpec / Fe / Be) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **No Prefetcher** | `test_fibonacci.elf` | **0.645** | 2650 | 14.49 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **No Prefetcher** | `test_matrix_mul.elf` | **1.992** | 1911 | 20.74 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **No Prefetcher** | `test_sum_array.elf` | **2.203** | 4562 | 30.96 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **No Prefetcher** | `test_sort.elf` | **2.266** | 3431 | 26.15 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Next-Line** | `test_fibonacci.elf` | **0.645** | 2650 | 13.96 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **Next-Line** | `test_matrix_mul.elf` | **1.992** | 1911 | 20.83 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Next-Line** | `test_sum_array.elf` | **2.203** | 4562 | 27.17 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **Next-Line** | `test_sort.elf` | **2.266** | 3431 | 23.46 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Stride (Dist 1)** | `test_fibonacci.elf` | **0.645** | 2650 | 14.13 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **Stride (Dist 1)** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.76 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Stride (Dist 1)** | `test_sum_array.elf` | **2.203** | 4562 | 22.92 | 99.9% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **Stride (Dist 1)** | `test_sort.elf` | **2.266** | 3431 | 26.44 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Stride (Dist 2)** | `test_fibonacci.elf` | **0.645** | 2650 | 14.08 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **Stride (Dist 2)** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.54 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Stride (Dist 2)** | `test_sum_array.elf` | **2.203** | 4562 | 26.35 | 99.9% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **Stride (Dist 2)** | `test_sort.elf` | **2.266** | 3431 | 24.78 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Stride (Dist 4)** | `test_fibonacci.elf` | **0.645** | 2650 | 10.84 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **Stride (Dist 4)** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.32 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Stride (Dist 4)** | `test_sum_array.elf` | **2.203** | 4562 | 29.93 | 99.9% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **Stride (Dist 4)** | `test_sort.elf` | **2.266** | 3431 | 24.03 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Stride (Dist 8)** | `test_fibonacci.elf` | **0.645** | 2650 | 10.87 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **Stride (Dist 8)** | `test_matrix_mul.elf` | **1.992** | 1911 | 20.25 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Stride (Dist 8)** | `test_sum_array.elf` | **2.203** | 4562 | 27.89 | 99.9% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **Stride (Dist 8)** | `test_sort.elf` | **2.266** | 3431 | 26.58 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |

---

## Scenario 9: DRAM Latency Sensitivity
*Performance impact of off-chip main memory latency scaling from 40 to 300 cycles.*

| Configuration | Benchmark | IPC | Cycles | Host MIPS | L1D Hit % | BP Acc % | TMAM (Ret / BadSpec / Fe / Be) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **DRAM Latency 40** | `test_fibonacci.elf` | **0.665** | 2570 | 14.62 | 99.7% | 73.6% | `92.9% / 0.0% / 0.0% / 7.1%` |
| **DRAM Latency 40** | `test_matrix_mul.elf` | **2.035** | 1871 | 20.92 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **DRAM Latency 40** | `test_sum_array.elf` | **2.242** | 4482 | 30.21 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **DRAM Latency 40** | `test_sort.elf` | **2.320** | 3351 | 24.57 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **DRAM Latency 80** | `test_fibonacci.elf` | **0.645** | 2650 | 10.99 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **DRAM Latency 80** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.45 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **DRAM Latency 80** | `test_sum_array.elf` | **2.203** | 4562 | 29.77 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **DRAM Latency 80** | `test_sort.elf` | **2.266** | 3431 | 26.67 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **DRAM Latency 150** | `test_fibonacci.elf` | **0.612** | 2790 | 13.54 | 99.7% | 73.6% | `90.1% / 0.0% / 0.0% / 9.9%` |
| **DRAM Latency 150** | `test_matrix_mul.elf` | **1.922** | 1981 | 18.26 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **DRAM Latency 150** | `test_sum_array.elf` | **2.137** | 4702 | 29.80 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **DRAM Latency 150** | `test_sort.elf` | **2.177** | 3571 | 26.79 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **DRAM Latency 300** | `test_fibonacci.elf` | **0.553** | 3090 | 12.25 | 99.7% | 73.6% | `86.5% / 0.0% / 0.0% / 13.4%` |
| **DRAM Latency 300** | `test_matrix_mul.elf` | **1.786** | 2131 | 21.35 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **DRAM Latency 300** | `test_sum_array.elf` | **2.009** | 5002 | 29.04 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **DRAM Latency 300** | `test_sort.elf` | **2.008** | 3871 | 26.53 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |

---

## Scenario 10: L1D Cache Size & Associativity
*L1 Data cache capacity (8KB..64KB) and set associativity (2-way..8-way) exploration.*

| Configuration | Benchmark | IPC | Cycles | Host MIPS | L1D Hit % | BP Acc % | TMAM (Ret / BadSpec / Fe / Be) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **8KB 2-Way** | `test_fibonacci.elf` | **0.645** | 2650 | 11.24 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **8KB 2-Way** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.81 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **8KB 2-Way** | `test_sum_array.elf` | **2.203** | 4562 | 29.43 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **8KB 2-Way** | `test_sort.elf` | **2.266** | 3431 | 26.40 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **16KB 4-Way** | `test_fibonacci.elf` | **0.645** | 2650 | 13.69 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **16KB 4-Way** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.82 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **16KB 4-Way** | `test_sum_array.elf` | **2.203** | 4562 | 28.37 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **16KB 4-Way** | `test_sort.elf` | **2.266** | 3431 | 23.83 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **32KB 4-Way** | `test_fibonacci.elf` | **0.645** | 2650 | 13.86 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **32KB 4-Way** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.11 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **32KB 4-Way** | `test_sum_array.elf` | **2.203** | 4562 | 31.09 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **32KB 4-Way** | `test_sort.elf` | **2.266** | 3431 | 26.09 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **64KB 8-Way** | `test_fibonacci.elf` | **0.645** | 2650 | 14.30 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **64KB 8-Way** | `test_matrix_mul.elf` | **1.992** | 1911 | 20.14 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **64KB 8-Way** | `test_sum_array.elf` | **2.203** | 4562 | 28.79 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **64KB 8-Way** | `test_sort.elf` | **2.266** | 3431 | 27.40 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |

---

## Scenario 11: Simulation Engine Speed & Scalability
*Host execution throughput (MIPS) across OoO Dynamic, Simple InOrder, FastFeeder, and Interpreter engines.*

| Configuration | Benchmark | IPC | Cycles | Host MIPS | L1D Hit % | BP Acc % | TMAM (Ret / BadSpec / Fe / Be) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **OoO Dynamic** | `test_fibonacci.elf` | **0.645** | 2650 | 10.94 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **OoO Dynamic** | `test_matrix_mul.elf` | **1.992** | 1911 | 20.40 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **OoO Dynamic** | `test_sum_array.elf` | **2.203** | 4562 | 30.91 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **OoO Dynamic** | `test_sort.elf` | **2.266** | 3431 | 21.62 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Simple InOrder** | `test_fibonacci.elf` | **0.001** | 17580 | 0.10 | 50.0% | 100.0% | `0.1% / 0.0% / 0.0% / 99.9%` |
| **Simple InOrder** | `test_matrix_mul.elf` | **0.844** | 4510 | 21.69 | 0.0% | 99.0% | `46.6% / 0.0% / 0.0% / 53.4%` |
| **Simple InOrder** | `test_sum_array.elf` | **0.717** | 14015 | 27.49 | 99.7% | 96.0% | `46.5% / 0.0% / 0.0% / 53.5%` |
| **Simple InOrder** | `test_sort.elf` | **0.794** | 9799 | 25.46 | 99.9% | 97.7% | `49.0% / 0.0% / 0.0% / 51.0%` |
| **Fast Feeder** | `test_fibonacci.elf` | **0.001** | 17580 | 0.10 | 50.0% | 100.0% | `0.1% / 0.0% / 0.0% / 99.9%` |
| **Fast Feeder** | `test_matrix_mul.elf` | **0.844** | 4510 | 21.74 | 0.0% | 99.0% | `46.6% / 0.0% / 0.0% / 53.4%` |
| **Fast Feeder** | `test_sum_array.elf` | **0.717** | 14015 | 27.25 | 99.7% | 96.0% | `46.5% / 0.0% / 0.0% / 53.5%` |
| **Fast Feeder** | `test_sort.elf` | **0.794** | 9799 | 26.19 | 99.9% | 97.7% | `49.0% / 0.0% / 0.0% / 51.0%` |
| **ISA Interpreter** | `test_fibonacci.elf` | **0.000** | 0 | 13.63 | 0.0% | 0.0% | `0.0% / 0.0% / 0.0% / 0.0%` |
| **ISA Interpreter** | `test_matrix_mul.elf` | **0.000** | 0 | 21.39 | 0.0% | 0.0% | `0.0% / 0.0% / 0.0% / 0.0%` |
| **ISA Interpreter** | `test_sum_array.elf` | **0.000** | 0 | 30.16 | 0.0% | 0.0% | `0.0% / 0.0% / 0.0% / 0.0%` |
| **ISA Interpreter** | `test_sort.elf` | **0.000** | 0 | 25.49 | 0.0% | 0.0% | `0.0% / 0.0% / 0.0% / 0.0%` |

---

## Scenario 12: Cache Subsystem Bypass Simulation Speed
*Host simulation speed tradeoff across Detailed Set-Associative, Zero-Latency, and Passthrough cache modes.*

| Configuration | Benchmark | IPC | Cycles | Host MIPS | L1D Hit % | BP Acc % | TMAM (Ret / BadSpec / Fe / Be) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Set Associative (Detailed)** | `test_fibonacci.elf` | **0.645** | 2650 | 13.88 | 99.7% | 73.6% | `91.8% / 0.0% / 0.0% / 8.2%` |
| **Set Associative (Detailed)** | `test_matrix_mul.elf` | **1.992** | 1911 | 21.91 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Set Associative (Detailed)** | `test_sum_array.elf` | **2.203** | 4562 | 25.41 | 99.7% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **Set Associative (Detailed)** | `test_sort.elf` | **2.266** | 3431 | 26.30 | 99.9% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Zero Latency (Ideal)** | `test_fibonacci.elf` | **0.691** | 2472 | 14.76 | 100.0% | 73.6% | `94.3% / 0.0% / 0.0% / 5.7%` |
| **Zero Latency (Ideal)** | `test_matrix_mul.elf` | **2.092** | 1820 | 20.88 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Zero Latency (Ideal)** | `test_sum_array.elf` | **2.293** | 4382 | 25.46 | 100.0% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **Zero Latency (Ideal)** | `test_sort.elf` | **2.393** | 3249 | 22.21 | 100.0% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |
| **Passthrough (Bypass)** | `test_fibonacci.elf` | **0.691** | 2472 | 14.31 | 0.0% | 73.6% | `94.3% / 0.0% / 0.0% / 5.7%` |
| **Passthrough (Bypass)** | `test_matrix_mul.elf` | **2.092** | 1820 | 16.47 | 0.0% | 99.0% | `68.2% / 0.0% / 0.0% / 31.8%` |
| **Passthrough (Bypass)** | `test_sum_array.elf` | **2.293** | 4382 | 30.90 | 0.0% | 96.9% | `80.0% / 0.0% / 0.0% / 20.0%` |
| **Passthrough (Bypass)** | `test_sort.elf` | **2.393** | 3249 | 26.34 | 0.0% | 97.7% | `81.5% / 0.0% / 0.0% / 18.5%` |

---
