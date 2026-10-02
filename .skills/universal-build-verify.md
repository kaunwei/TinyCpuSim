---
name: universal-build-verify
description: "Universal build, test, ubench invariant verification, and auto-commit SOP for unattended background workers in TinyCpuSim."
---

# Universal Build, Test & Verification SOP (Background Worker)

This document is the **mandatory standard operating procedure** that the unattended background Worker AI MUST strictly follow when executing tasks popped from `tasks.txt`.

---

## 1. Strict Security Constraints (安全約束)

1. **NO Global System Modifications**:
   - Under NO circumstances run `sudo`, `apt`, `apt-get`, `yum`, `dnf`, `pacman`, or any host system-level package manager.
2. **Standard Installed Tool Paths**:
   - Use standard tool paths directly (e.g. `/usr/bin/python3`, `cmake`, `ninja`). Avoid unnecessary environment variable overrides unless strictly required.
3. **Local Environment Only**:
   - Dependencies and build files must be managed locally within the project (`build/`).
4. **Missing System Dependencies**:
   - If a required tool or compiler is absent, **DO NOT attempt to install it via sudo**.
   - Mark the task as `[BLOCKED: Missing dependency <name>]`, output the remediation command for the human in `progress.log`, and terminate immediately.

---

## 2. Implementation, Build & Verification Workflow

### Step 2.1: Code Implementation
- Open only the specified `TARGET` files.
- Implement the requested feature, bug fix, or refactor cleanly according to the `ACTION` specification.
- Follow Test-Driven Development (TDD) and vertical slicing. Modern C++20 standard, strict RAII, zero compiler warnings.

### Step 2.2: Compilation & Build Verification
- Execute project build script or CMake/Ninja directly:
  ```bash
  ./scripts/01_build.sh
  ```
  *(or `cmake -B build -S . -GNinja && cmake --build build --parallel`)*
- Must compile cleanly with **zero warnings and zero errors**.

### Step 2.3: Automated Test Execution
- Run unit and regression tests:
  ```bash
  ./scripts/02_run_tests.sh
  ```
  *(or `ctest --test-dir build --output-on-failure`)*
- **All tests must pass (100% green, 168/168 tests pass).**

### Step 2.4: Microarchitectural Invariant Verification (uarch / gem5)
- When modifying pipeline, predictor, ROB, or LSU components, verify microbenchmarks:
  ```bash
  /usr/bin/python3 scripts/report_ubench_perf.py --suite all
  ```
  *(or `./scripts/03_run_ubench.sh`)*
- If golden gem5 comparison is specified in task:
  ```bash
  ./scripts/05_compare_gem5.sh --all
  ```
- **Zero Drift Gate**: Invariants must achieve `<1%` delta against empirical references.

---

## 3. Error Retry Limit (修復上限原則)

When compilation, linking, tests, or microbenchmarks fail:
1. **Maximum 3 Fix Attempts**:
   - Attempt 1: Read the exact error trace, hypothesize the root cause, apply a surgical fix, rebuild and retest.
   - Attempt 2: If secondary errors occur, re-evaluate and refine fix.
   - Attempt 3: Final attempt to resolve any remaining issues.
2. **Immediate Stop on Exceeded Limit**:
   - If the task cannot pass after the 3rd attempt, **IMMEDIATELY STOP**.
   - Do NOT enter an infinite loop.
   - Mark the task as `[BLOCKED]` in `progress.log` with the exact failure reason and exit with error status.

---

## 4. Completion Standard & Evidence Logging (結案標準)

Upon 100% verification success:
1. **Auto Git Commit**:
   ```bash
   git add <modified-target-files>
   git commit -m "feat/fix: <task title> [TASK-ID]"
   ```
2. **Prepend Structured Summary to `progress.log`**:
   Write a concise summary (strictly **≤ 10 lines**) at the **VERY TOP** of `progress.log`:

```text
================================================================================
[SUCCESS] TASK_ID: <Task Title>
Time: <YYYY-MM-DD HH:MM:SS> | Commit: <Short Hash> | Duration: <Xs>
Changes: <List of modified files and functions>
Verification: Build PASS, Tests PASS (168 passed), Invariant PASS (<1% delta)
Notes: <Brief note or N/A>
================================================================================
```

If blocked, prepend the `[BLOCKED]` report:
```text
================================================================================
[BLOCKED] TASK_ID: <Task Title>
Time: <YYYY-MM-DD HH:MM:SS> | Status: BLOCKED after 3 attempts
Failure Reason: <Exact error summary>
Action Needed for Human: <Clear instructions for the human operator>
================================================================================
```
