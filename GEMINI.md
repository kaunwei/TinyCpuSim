# Agent Rules & Treaties (TinyCpuSim & 24h Unattended Framework)

This document defines the mandatory rules, operational guardrails, and conventions that all AI agents MUST strictly observe when executing commands, modifying code, or performing workflows in this repository.

---

## 1. 24h Unattended Dual-Terminal Workflow

This repository operates on a **Dual-Terminal Architecture** separating interactive architectural planning from background unattended execution:

```
┌─────────────────────────────────────────────────────────────┐
│                 Terminal A: Interactive Architect           │
│  • Discuss architecture & requirements with user.           │
│  • Decompose goals into single-line atomic English tasks.   │
│  • Autonomously manages .worker.env (models, ext dirs).    │
│  • Inspect progress.log (top <=10 lines) for acceptance.    │
└──────────────────────────────┬──────────────────────────────┘
                               │ (FIFO Queue)
┌──────────────────────────────▼──────────────────────────────┐
│                 Terminal B: Unattended Background Worker    │
│  • Pure zero-argument execution (just run ./worker.sh).     │
│  • Auto-sources .worker.env (ADDITIONAL_DIRS=../gem5).      │
│  • Stateless CLI process (agy --model gemini-3.7-flash-low).│
│  • Follows .skills/universal-build-verify.md.               │
│  • Runs builds, unit tests (168/168 pass), ubench delta.    │
│  • Max 3 repair attempts -> auto rollback on block.         │
│  • Auto Git Commit & prepends <=10 lines to progress.log.   │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. Strict Inter-AI English Protocol

To optimize token efficiency (50-70% savings) and ensure maximum instruction-following precision across weaker models:
- **Human-AI Discussions**: Traditional Chinese (繁體中文) is welcomed in Terminal A.
- **All Inter-AI Files & Artifacts**: MUST be written in **Strictly English**:
  - `tasks.txt`
  - `progress.log`
  - `worker.status`
  - `tasks.done`
  - Git Commit messages and code comments.

---

## 3. Atomic Task Format Guide (for Terminal A AI)

When Terminal A decomposes user requests into `tasks.txt`, each task MUST occupy exactly one line and adhere to this structured contract:

```text
TASK-XXX | TARGET: <file_paths> | ACTION: <precise implementation details> | VERIFY: <test command> | CONSTRAINTS: <scope limits>
```

### Examples:
```text
TASK-001 | TARGET: src/core/rob.cpp, tests/unit/rob_test.cpp | ACTION: Add squashing logic on mispredicted branches | VERIFY: ./scripts/01_build.sh && ./scripts/02_run_tests.sh | CONSTRAINTS: C++20 strict RAII, zero warnings
TASK-002 | TARGET: src/core/branch_predictor.cpp | ACTION: Calibrate BPU counter saturation | VERIFY: /usr/bin/python3 scripts/report_ubench_perf.py --suite bp | CONSTRAINTS: <1% delta against gem5 golden counters
```

---

## 4. Verification & Safe Git Log Rules (Zero Context Pollution)

To keep Terminal A's context window clean and avoid context exhaustion:
1. **Primary Acceptance**: Terminal A only reads the top 10 lines of `progress.log` (`head -n 15 progress.log`).
2. **Safe Git Log Whitelist (If deeper check is needed)**:
   - `git log -n 1 --stat`
   - `git show --stat <commit-hash>`
   - `git log --oneline -n 5`
3. **STRICTLY FORBIDDEN in Terminal A**:
   - ❌ Bare `git log` (dumps unbounded commit history).
   - ❌ `git log -p` / bare `git show <hash>` (dumps hundreds of lines of code diffs).
   - ❌ `git diff main...HEAD` (full diff).

---

## 5. Branch Strategy: Linear Git Rebase & Auto-Cleanup

All batch feature development should happen on integration branches and integrate linearly into `main`:

```bash
# 1. Rebase feature branch onto latest main
git checkout feature/<batch-name>
git rebase main

# 2. Fast-forward merge into main
git checkout main
git merge --ff-only feature/<batch-name>

# 3. Safely delete the feature branch after confirmed merged
git branch -d feature/<batch-name>
```

---

## 6. Mandatory Command Execution & Git Guardrails

Whenever executing commands in this workspace, the agent MUST obey the following safety rules:
- **Tool Paths**: Always use standard installed default paths (e.g. `/usr/bin/python3`). Avoid unnecessary environment variable overrides.
- **Commit Frequency**: Commit after every distinct task or unit of work.
- **No Destructive Git Operations**: Under NO circumstances should the agent run:
  - `git push` / `git push --force`
  - `git reset --hard` (except worker.sh automated isolated rollback)
  - `git clean -f` / `git clean -fd`
  - `git branch -D`
  unless specifically and explicitly requested by the user.
- **No Host-Level Modifying Commands**: Under NO circumstances run `sudo`, `apt`, `yum`, `dnf`, `pacman`.

---

## 7. Microarchitectural Invariant & Engineering Treaties

- **Microarchitectural Correlation**: All microbenchmark invariants must achieve `<1%` delta against gem5 empirical references (`tests/uarch/golden_counters.json`).
- **Test-Driven Development (TDD)**: Follow Red-Green-Refactor cycle. Unit tests must pass 100%.
- **Deep Modules**: Modern C++20 standard, strict RAII, zero compiler warnings.
