---
name: universal-build-verify
description: "Universal multi-archetype build, test, ubench invariant verification, honest retry tracking, and auto-commit SOP for unattended background workers in TinyCpuSim."
---

# Universal Multi-Archetype Execution & Verification SOP (Background Worker)

This document is the **mandatory standard operating procedure** that the unattended background Worker AI MUST strictly follow when executing tasks popped from `tasks.txt`.

---

## 1. Strict Security & Anti-Rot Constraints (安全與防腐門禁)

1. **NO Global System Modifications**:
   - Under NO circumstances run `sudo`, `apt`, `apt-get`, `yum`, `dnf`, `pacman`, or any host system-level package manager.
2. **Standard Tool Paths**:
   - Always invoke standard tool paths (`/usr/bin/python3`, `/usr/bin/cmake`, `/usr/bin/ninja`, `/usr/bin/g++`).
3. **Zero Compiler / Lint Warnings (零警告門禁)**:
   - Code must build cleanly with C++20/C++17 standard, strict RAII, zero compiler warnings (`-Wall -Wextra`), and zero linker errors.
4. **Interface Immutability**:
   - Do not alter public module interfaces unless explicitly specified in the task.

---

## 2. Dynamic Task Archetype Execution Workflows

### Archetype A: Code Implementation & Refactoring (`[CODE]`)
1. **Single-Seam Freedom (接縫自由實作)**: As long as the task operates within a single architectural module or cohesive seam (e.g. `ReorderBuffer`, `LoadStoreQueue`, `BranchPredictor`, `SliceManager`), there is **NO line-of-code limit**. Worker is fully authorized to write hundreds of lines of implementation, helper structs, and tests.
2. **Cross-Subsystem Rejection (跨子系統過載拒絕)**: If a task requires simultaneous, uncoordinated modifications across multiple independent subsystems, emit `[NEED_DECOMPOSITION]` with proposed subtasks and exit cleanly.
3. **Local Autonomy (局部決策權)**: Worker has full authority on internal algorithms, private helper naming, and data structures.
4. **Build & Test Verification**:
   - Build: `./scripts/01_build.sh` (must be 0 warnings, 0 errors).
   - Unit tests: `./scripts/02_run_tests.sh` (100% passing).
   - Microbenchmark invariants: `/usr/bin/python3 scripts/report_ubench_perf.py --suite all` (`<1%` delta).

### Archetype B: Architectural & Spec Research (`[RESEARCH]`)
1. **Source Inspection**: Examine primary sources (e.g. `../gem5`, ARM ARM architecture manuals, code comments).
2. **Report Generation**: Write findings to designated markdown files under `docs/` or `.scratch/`.
3. **Structured Verification**: Ensure cited file paths and symbols are clickable links (`file:///path/to/file#L10-L30`).

### Archetype C: Performance & Benchmark Characterization (`[BENCHMARK]`)
1. **Execution**: Run designated benchmarks or sweeps with reproducible CLI flags.
2. **Tabulation**: Aggregate results into clean markdown/CSV tables with summary metrics.

### Archetype D: Diagnostic & Bug Isolation (`[DIAGNOSTIC]`)
1. **Reproduction**: Create minimal failing test cases or reproduce error traces.
2. **Root Cause Analysis**: Isolate the exact failing branch with evidence.

---

## 3. Strict Zero-Guessing Honest Treaty (嚴禁通靈硬猜之誠實公約)

Across all task archetypes (Code, Research, Benchmarking, Diagnostics), Worker MUST strictly observe:

1. **Anti-Hallucination / Zero Speculation (看不懂絕不硬猜)**:
   - If legacy code, obscure macros, complex math, or undocumented behaviors are ambiguous, Worker **MUST NOT guess or invent intent**.
   - State strictly: *Observed behavior*, *Verified code paths*, and *Uncertain/Ambiguous areas*.
2. **Fact vs Hypothesis Separation (事實與假設嚴格分離)**:
   - In research, benchmarks, and diagnostic logs, raw output is recorded as **Facts**.
   - Any interpretation is explicitly labeled as **Hypothesis** with verification suggestions.
3. **Transparent Evidence**:
   - Every completed task must provide reproducible proof (test command pass, benchmark output summary, or linked file locations).

---

## 4. Error Retry Limit & Intermediate Clean Reset (修復上限與中間重置)

When build, compilation, tests, or scripts fail:
1. **Maximum 3 Fix Attempts**:
   - **Attempt 1**: Analyze error trace, apply surgical fix, rebuild/retest.
   - **Attempt 2**: If Attempt 1 made things worse, run `git restore <file>` to clean intermediate state back to baseline before trying an alternative fix.
   - **Attempt 3**: Final attempt.
2. **Immediate Stop on 3rd Failure**:
   - If unable to pass after 3 attempts, STOP immediately. Do NOT enter an infinite loop.
   - Mark task as `[BLOCKED]` in `progress.log` and pause cleanly.

---

## 5. Honest Escalation Protocol (誠實求助與務實推進協議)

### 3-Tier Escalation:
- **Tier 1 (Local Choices)**: Private helpers, internal data structures, data presentation formatting ➔ Worker decides autonomously.
- **Tier 2 (Recoverable Errors)**: Compiler/test errors, script typos ➔ Self-heal within 3 attempts.
- **Tier 3 (Fatal Blockers)**: Spec contradictions, missing host dependencies, or 3 failed retries ➔ Emit `[NEED_GUIDANCE]` or `[BLOCKED]` and pause cleanly.

### Best-Effort with Note (非阻塞標記推進):
For minor ambiguities that do not threaten core integrity:
- Make a standard, idiomatic engineering decision.
- Finish the task, verify, and commit normally.
- Append a note in `progress.log`: `Notes: [Brief explanation of the choice made, allowing Architect to review/refine]`.
- DO NOT halt the unattended pipeline for non-critical ambiguities.

---

## 6. Completion Standard & Evidence Logging (結案標準與超精簡協議)

### Ultra-Concise Inter-AI Information Protocol (AI 間極簡資訊交換規範):
1. **Length Limit**:
   - `progress.log` entry MUST NOT exceed **15 lines**.
   - Dedicated report files (e.g. `docs/reports/*.md`) MUST begin with an **Executive Summary <= 30 lines** (key metrics, pass/fail table, raw pointers).
2. **Dense & High-Signal**: No polite greetings, narrative essays, or verbose explanations. Only include:
   - Target files & line references (`file:///path/to/file#L10-L25`)
   - Exact command exit codes & execution time
   - Measurable metrics (e.g. `185/185 passed, 0 warnings`)
3. **Planner On-Demand Inspection Right (Planner 保留按需抽查權)**:
   - The worker's summary is designed for fast acceptance without token bloat.
   - Planner retains the sovereign right to inspect underlying source files, run targeted commands, or check git stats whenever summary information is deemed insufficient.

Upon 100% verification success:
1. **Auto Git Commit**:
   ```bash
   git add <modified-target-files>
   git commit -m "feat/fix: <task title> [TASK-ID]"
   ```
2. **Prepend Structured Summary & Attempt Trace to `progress.log` (Strictly <= 15 lines)**:
```text
================================================================================
[SUCCESS] TASK-XXX: <Task Title>
Time: <YYYY-MM-DD HH:MM:SS> | Duration: <Xs> | Attempts: <N>/3 | Confidence: <HIGH|MED|LOW>
Task Granularity: <Level 1|Level 2|Level 3>
Changes: <Modified files / Report path>
Verification: Build PASS, Tests PASS (185 passed), Invariant PASS (<1% delta)
Attempt Trace:
  • Attempt 1: <PASS or failure reason with exact error summary>
  • Attempt 2: <Fix applied, if any>
Granularity Feedback: <Worker observation on module granularity>
================================================================================
```
