# TinyArmSim

## Agent Skills & Engineering Operating Guidelines

### 1. Dual-Skill Library & Automatic Discovery
All engineering and productivity skills are tracked directly in `.agents/skills/` (registered in `.agents/skills.json`):
- **Architect Skills**: `.agents/skills/architect/` (`calibrate-task-granularity`, `handle-worker-guidance`, `score-worker-performance`, `watch-worker-queue`).
- **Worker Skills**: `.agents/skills/worker/` (`manage-worker-notes`, `record-attempt-trace`).
- **Core Engineering**: `.agents/skills/engineering/` (`tdd`, `implement`, `codebase-design`, `code-review`, `diagnosing-bugs`, `domain-modeling`, `wayfinder`, `uarch-perf-correlation`, `setup-unattended-workflow`, `to-spec`, `to-tickets`, `triage`, `resolving-merge-conflicts`, `prototype`, `research`, `improve-codebase-architecture`, `wizard`, `ask-matt`, `grill-with-docs`, `implement-spec`, `retro`, `pr`, `setup-matt-pocock-skills`).
- **Productivity & Review**: `.agents/skills/productivity/` (`configure-unattended-worker`, `grilling`, `grill-me`, `handoff`, `teach`, `to-questionnaire`, `wait-what`, `writing-for-agents`).
- **Misc**: `.agents/skills/misc/` (`setup-pre-commit`, `migrate-to-shoehorn`, `scaffold-exercises`, `git-guardrails-claude-code`).

### 2. Microarchitectural Performance Correlation (`uarch-perf-correlation`)
- **Layer A Focus**: Performance calibration is conducted exclusively via isolated C++ subsystem microbenchmarks (`tests/uarch/*_ubench_test.cpp`). Full ELF binaries are NOT used for component invariant calibration due to pipeline noise.
- **Empirical Ground Truth**: Comparison baselines come strictly from real gem5 execution stats cached in [`tests/uarch/golden_counters.json`](file:///home/kw/workspace/TinyCpuSim/tests/uarch/golden_counters.json).
- **Comparator Tool**: Run [`scripts/report_ubench_perf.py`](file:///home/kw/workspace/TinyCpuSim/scripts/report_ubench_perf.py) with `--suite bp|core|lsu|rob|cache|all`.
- **Zero Drift Gate**: All microbenchmark invariants must achieve `<1%` delta against gem5 empirical references with 100% functional regression pass.

### 3. TDD & Codebase Design
- Follow strict TDD Red-Green-Refactor cycles and vertical slicing.
- Design deep modules with clean, agreed seams (`CompositeBranchPredictor`, `FetchUnit`, `LoadStoreQueue`, `ReorderBuffer`, `OOOCore`, `SliceManager`).
- Modern C++20 standard, strict RAII, and zero compiler warnings.
- Tool Paths: Always use standard default tool paths (e.g., `/usr/bin/python3`, standard compiler binaries). Avoid unnecessary environment variable overrides.

### 4. Project Tracking & Documentation
- Issues and specs: Local Markdown under `.scratch/` (see `docs/agents/issue-tracker.md`).
- Canonical 5-role triage labels: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`.
- Single-context repo layout: `CONTEXT.md` + `docs/adr/`.

---

## 24h Unattended Dual-Terminal Workflow & Dual-Track Offloading

This repository operates on a **Physical Dual-Terminal Architecture** separating interactive architectural planning from background unattended execution:

```
┌────────────────────────────────────────────────────────────────────────┐
│               Terminal A: Interactive Architect (In-Session)           │
│  • Discuss architecture & requirements with user.                      │
│  • [Track 1 Offload] Immediate dirty work / exploratory fact-finding   │
│    is delegated to In-Session Subagents (auto-notified on completion). │
│  • Decompose batch goals into single-line atomic English tasks.        │
│  • Writes tasks to tasks.txt and IMMEDIATELY yields control.           │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ (tasks.txt FIFO Queue)
┌───────────────────────────────────▼────────────────────────────────────┐
│          Terminal B: Background Worker Terminal (Mounted Daemon)       │
│  • User mounts and runs ./worker.sh in a separate terminal / tmux.     │
│  • [Track 2 Offload] Unattended 24h batch execution & test suites.     │
│  • Pure zero-argument execution (auto-sources .worker.env).            │
│  • Stateless CLI process (agy --model gemini-3.7-flash-low).           │
│  • Follows .skills/universal-build-verify.md.                          │
│  • Runs builds, unit tests (185/185 pass), ubench delta (<1%).         │
│  • Max 3 repair attempts -> auto rollback on block.                    │
│  • Auto Git Commit & prepends <=15 lines to progress.log.              │
└────────────────────────────────────────────────────────────────────────┘
```

### Dual-Track Offloading Protocol (雙軌卸載協定):
1. **Track 1: In-Session Immediate Offloading (即時探勘卸載)**:
   - When Planner needs to explore hundreds of lines of code, analyze configs, or gather facts *during the ongoing conversation*, Planner invokes an in-session **Subagent** (`invoke_subagent`).
   - The subagent digests the heavy content and returns a concise summary. The system automatically wakes Planner up upon completion (no `sleep` or polling needed).
2. **Track 2: Cross-Terminal Batch Offloading & Sentinel Subagent Wakeup (跨終端批次與哨兵喚醒)**:
   - When a plan or batch of features is ready, Planner queues tasks into `tasks.txt`.
   - **Sentinel Subagent Wakeup**: If proactive completion notification is desired without freezing Terminal A, Planner launches a lightweight **Queue Sentinel Subagent** (`invoke_subagent` with `bash scripts/watch_worker.sh 3600`).
   - Planner immediately yields control to Human. Terminal A remains 100% interactive.
   - When Terminal B finishes all tasks, the Sentinel Subagent detects completion via zero-token script exit and triggers a **system wakeup event**, allowing Planner to deliver the final acceptance report automatically.

### Strict Inter-AI English Protocol
- All tasks in `tasks.txt`, summaries in `progress.log`, status in `worker.status`, and git commits MUST be in **strictly English** to conserve tokens and maximize execution precision.

### Dynamic Granularity Ladder (動態任務顆粒度)
Terminal A dynamically sizes tasks into `tasks.txt` based on `docs/agents/granularity-profile.json`:

| Level | Granularity | Scope & Action |
| :---: | :--- | :--- |
| **Level 1** | **Coarse (Seam / Feature)** | Specify public interface & verification test. Worker autonomously creates internal helpers & implementation. |
| **Level 2** | **Medium (Component Slice)** | Break down into discrete module units (e.g. data model vs processor). |
| **Level 3** | **Micro (Step / Function)** | Precise function-level instructions. Used only when worker requested guidance or previous attempt struggled. |

Task Format:
`TASK-XXX | LEVEL: 1 | TARGET: <files> | ACTION: <concrete logic> | VERIFY: <test command> | CONSTRAINTS: <bounds>`

### Honest Worker Treaty & Escalation (誠實工人公約)
1. **Single-Seam Freedom (接縫自由實作)**: As long as a task operates on a single architectural module/seam, there is **NO line-of-code limit**. Worker has full autonomy on internal helpers, data structures, and tests.
2. **Cross-Subsystem Rejection (跨子系統過載拒絕)**: If a task demands simultaneous, uncoordinated changes across multiple decoupled subsystems, Worker emits `[NEED_DECOMPOSITION]` with proposed subtasks.
3. **3-Tier Escalation & Best-Effort with Note**:
   - *Tier 1 (Local choices)*: Worker decides autonomously.
   - *Tier 2 (Recoverable errors)*: Worker self-heals within 3 retries (with `git restore` clean intermediate resets).
   - *Tier 3 (Fatal blockers)*: Worker emits `[NEED_GUIDANCE]` or `[BLOCKED]` and pauses.
   - *Minor ambiguities*: Advance with Best-Effort and leave a non-blocking `Notes:` entry in `progress.log`.

### Safe Git Log Rules for Acceptance AI
- Allowed: `git log -n 1 --stat`, `git show --stat <commit-hash>`, `git log --oneline -n 5`
- Prohibited: Bare `git log`, `git log -p`, full `git diff` (prevents context explosion).

### Git Rebase Branch Integration
```bash
git checkout feature/<batch-name>
git rebase main
git checkout main
git merge --ff-only feature/<batch-name>
git branch -d feature/<batch-name>
```
