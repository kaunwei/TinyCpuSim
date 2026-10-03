---
name: universal-build-verify
description: "Universal build, test, ubench invariant verification, honest retry tracking, and auto-commit SOP for unattended background workers in TinyCpuSim."
---

# Universal Build, Test & Verification SOP (Background Worker)

This document is the **mandatory standard operating procedure** that the unattended background Worker AI MUST strictly follow when executing tasks popped from `tasks.txt`.

---

## 1. Strict Security & Anti-Rot Constraints (安全與防腐門禁)

1. **NO Global System Modifications**:
   - Under NO circumstances run `sudo`, `apt`, `apt-get`, `yum`, `dnf`, `pacman`, or any host system-level package manager.
2. **Standard Tool Paths**:
   - Use standard tool paths (`/usr/bin/python3`, `cmake`, `ninja`).
3. **Zero Compiler / Lint Warnings (零警告門禁)**:
   - Code must build cleanly with C++20 standard, strict RAII, zero compiler warnings (`-Wall -Wextra`), and zero linker errors.
4. **Interface Immutability**:
   - Do not alter public module interfaces unless explicitly specified in the task.

---

## 2. Implementation, Build & Verification Workflow

### Step 2.0: Consult Notes & Real-Time Step Logging
- Check `.worker.notes.md` for project-specific quirks.
- Before invoking any command or modifying files, print an explicit line:
  - `[EXEC] Modifying <filename>...`
  - `[EXEC] Running build command: <command>`
  - `[EXEC] Running test command: <command>`
  - `[EXEC] Running git commit...`

### Step 2.1: Code Implementation & Single-Seam Autonomy
- **Single-Seam Freedom (接縫自由實作)**: As long as the task operates within a single architectural module or cohesive seam (e.g. `ReorderBuffer`, `LoadStoreQueue`, `BranchPredictor`, `SliceManager`), there is **NO line-of-code limit**. Worker is fully authorized to write hundreds of lines of implementation, helper structs, and tests.
- **Cross-Subsystem Rejection (跨子系統過載拒絕)**: If a task requires simultaneous, uncoordinated modifications across multiple independent subsystems (which should be orchestrated by the Planner), emit `[NEED_DECOMPOSITION]` with proposed subtasks and exit cleanly.
- **Local Autonomy (局部決策權)**: Worker has full authority on internal algorithms, private helper naming, and data structures. Do not halt for minor internal choices.

### Step 2.2: Compilation & Build Verification
- Execute project build:
  ```bash
  ./scripts/01_build.sh
  ```
  *(or `cmake --build build -j$(nproc)` / `ninja -C build`)*
- Must compile cleanly with **zero warnings and zero errors**.

### Step 2.3: Automated Test Execution
- Run unit and regression tests:
  ```bash
  ./scripts/02_run_tests.sh
  ```
  *(or `ctest --test-dir build --output-on-failure`)*
- **All tests must pass (100% green, 178/178 tests pass).**

### Step 2.4: Microarchitectural Invariant Verification (uarch / gem5)
- When modifying pipeline, predictor, ROB, or LSU components, verify microbenchmarks:
  ```bash
  /usr/bin/python3 scripts/report_ubench_perf.py --suite all
  ```
- **Zero Drift Gate**: Invariants must achieve `<1%` delta against empirical references.

---

## 3. Error Retry Limit & Intermediate Clean Reset (修復上限與中間重置)

When build, compilation, or tests fail:
1. **Maximum 3 Fix Attempts**:
   - **Attempt 1**: Analyze error trace, apply surgical fix, rebuild and retest.
   - **Attempt 2**: If Attempt 1 made things worse, run `git restore <file>` to reset back to baseline before trying an alternative fix.
   - **Attempt 3**: Final attempt.
2. **Immediate Stop on 3rd Failure**:
   - If unable to pass after 3 attempts, STOP immediately. Do NOT enter an infinite loop.
   - Mark the task as `[BLOCKED]` in `progress.log` and exit.

---

## 4. Honest Escalation Protocol (誠實求助與務實推進協議)

### 3-Tier Escalation:
- **Tier 1 (Local Choices)**: Private helpers, internal data structures ➔ Worker decides autonomously.
- **Tier 2 (Recoverable Errors)**: Compiler/test errors ➔ Self-heal within 3 attempts.
- **Tier 3 (Fatal Blockers)**: Direct spec contradiction, missing sudo dependencies, or 3 failed retries ➔ Emit `[NEED_GUIDANCE]` or `[BLOCKED]` and exit cleanly.

### Best-Effort with Note (非阻塞標記推進):
For minor ambiguities (e.g. default return values or standard error types):
- Make a standard, idiomatic engineering decision.
- Finish the task, pass tests, and commit normally.
- Append a note in `progress.log`: `Notes: [Brief explanation of the choice made, allowing Architect to refine in future task if desired]`.
- DO NOT halt the unattended pipeline for non-critical ambiguities.

---

## 5. Completion Standard & Evidence Logging (結案標準)

Upon 100% verification success:
1. **Auto Git Commit**:
   ```bash
   git add <modified-target-files>
   git commit -m "feat/fix: <task title> [TASK-ID]"
   ```
2. **Prepend Structured Summary & Attempt Trace to `progress.log`**:
```text
================================================================================
[SUCCESS] TASK-XXX: <Task Title>
Time: <YYYY-MM-DD HH:MM:SS> | Duration: <Xs> | Attempts: <N>/3 | Confidence: <HIGH|MEDIUM|LOW>
Task Granularity: <Level 1|Level 2|Level 3>
Changes: <List of modified files>
Verification: Build PASS, Tests PASS (178 passed), Invariant PASS (<1% delta)

Attempt Trace:
  • Attempt 1: <PASS or failure reason>
  • Attempt 2: <Fix applied, if any>

Granularity Feedback: 
  <Worker observation on whether this module was comfortable at current granularity>
================================================================================
```
