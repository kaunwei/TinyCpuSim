# Agent Rules & Treaties (TinyCpuSim & 24h Unattended Framework)

This document defines the mandatory rules, operational guardrails, and conventions that all AI agents MUST strictly observe when executing commands, modifying code, or performing workflows in this repository.

---

## 1. 24h Unattended Dual-Terminal Workflow

This repository operates on a **Dual-Terminal Architecture** separating interactive architectural planning from background unattended execution:

```
┌─────────────────────────────────────────────────────────────┐
│                 Terminal A: Interactive Architect           │
│  • Discuss architecture & requirements with user.           │
│  • Decompose goals into atomic tasks using Granularity Ladder.
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
│  • Runs builds, unit tests (178/178 pass), ubench delta.    │
│  • Single-seam freedom, honest escalation [NEED_GUIDANCE].  │
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

## 3. Dynamic Granularity Ladder (動態任務顆粒度)

Terminal A dynamically sizes tasks into `tasks.txt` based on `docs/agents/granularity-profile.json`:

| Level | Granularity | Scope & Action |
| :---: | :--- | :--- |
| **Level 1** | **Coarse (Seam / Feature)** | Specify public interface & verification test. Worker autonomously creates internal helpers & implementation. |
| **Level 2** | **Medium (Component Slice)** | Break down into discrete module units (e.g. data model vs processor). |
| **Level 3** | **Micro (Step / Function)** | Precise function-level instructions. Used only when worker requested guidance or previous attempt struggled. |

### Task Format:
```text
TASK-XXX | LEVEL: 1 | TARGET: <file_paths> | ACTION: <precise implementation details> | VERIFY: <test command> | CONSTRAINTS: <scope limits>
```

---

## 4. Honest Worker Treaty & Escalation (誠實工人公約)

1. **Single-Seam Freedom (接縫自由實作)**: As long as a task operates on a single architectural module/seam, there is **NO line-of-code limit**. Worker has full autonomy on internal helpers, data structures, and tests.
2. **Cross-Subsystem Rejection (跨子系統過載拒絕)**: If a task demands simultaneous, uncoordinated changes across multiple decoupled subsystems, Worker emits `[NEED_DECOMPOSITION]` with proposed subtasks.
3. **3-Tier Escalation & Best-Effort with Note**:
   - *Tier 1 (Local choices)*: Worker decides autonomously.
   - *Tier 2 (Recoverable errors)*: Worker self-heals within 3 retries (with `git restore` clean intermediate resets).
   - *Tier 3 (Fatal blockers)*: Worker emits `[NEED_GUIDANCE]` or `[BLOCKED]` and pauses.
   - *Minor ambiguities*: Advance with Best-Effort and leave a non-blocking `Notes:` entry in `progress.log`.

---

## 5. Dual-Skill Ecosystem

- **Architect Skills** (`.agents/skills/architect/`): `/calibrate-task-granularity`, `/handle-worker-guidance`, `/score-worker-performance`.
- **Worker Skills** (`.agents/skills/worker/`): `/manage-worker-notes`, `/record-attempt-trace`.
- **Core Engineering**: `.agents/skills/engineering/` (`/tdd`, `/codebase-design`, `/domain-modeling`, `/diagnosing-bugs`, `/uarch-perf-correlation`, `/setup-unattended-workflow`).
- **Productivity & Review**: `.agents/skills/productivity/` (`/grilling`, `/handoff`, `/teach`, `/to-questionnaire`, `/wait-what`).

---

## 6. Verification & Safe Git Log Rules (Zero Context Pollution)

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

## 7. Branch Strategy: Linear Git Rebase & Auto-Cleanup

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

## 8. Microarchitectural Invariant & Engineering Treaties

- **Microarchitectural Correlation**: All microbenchmark invariants must achieve `<1%` delta against gem5 empirical references (`tests/uarch/golden_counters.json`).
- **Test-Driven Development (TDD)**: Follow Red-Green-Refactor cycle. Unit tests must pass 100%.
- **Deep Modules**: Modern C++20 standard, strict RAII, zero compiler warnings.
