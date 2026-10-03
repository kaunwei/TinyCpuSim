# TinyArmSim

## Agent Skills & Engineering Operating Guidelines

### 1. Dual-Skill Library & Automatic Discovery
All engineering and productivity skills are tracked directly in `.agents/skills/` (registered in `.agents/skills.json`):
- **Architect Skills**: `.agents/skills/architect/` (`calibrate-task-granularity`, `handle-worker-guidance`, `score-worker-performance`).
- **Worker Skills**: `.agents/skills/worker/` (`manage-worker-notes`, `record-attempt-trace`).
- **Core Engineering**: `.agents/skills/engineering/` (`tdd`, `implement`, `codebase-design`, `code-review`, `diagnosing-bugs`, `domain-modeling`, `wayfinder`, `uarch-perf-correlation`, `setup-unattended-workflow`, `to-spec`, `to-tickets`, `triage`, `resolving-merge-conflicts`, `prototype`, `research`, `improve-codebase-architecture`, `wizard`, `ask-matt`).
- **Productivity & Review**: `.agents/skills/productivity/` (`grilling`, `handoff`, `teach`, `to-questionnaire`, `wait-what`, `writing-for-agents`).
- **Misc**: `.agents/skills/misc/` (`setup-pre-commit`, `migrate-to-shoehorn`, `scaffold-exercises`).

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

## 24h Unattended Dual-Terminal Workflow

This repository operates on a **Dual-Terminal Architecture** separating interactive architectural planning from background unattended execution:

### 1. Dual-Terminal Architecture
- **Terminal A (Interactive Architect)**: Discuss with human in Chinese/English, groom requirements, write atomic English tasks to `tasks.txt`, and inspect `progress.log` for acceptance.
- **Terminal B (Background Worker)**: Runs `./worker.sh`. Stateless process per task, executes build (`./scripts/01_build.sh`), test (`./scripts/02_run_tests.sh`), microbenchmark invariants (`/usr/bin/python3 scripts/report_ubench_perf.py --suite all`), and auto-commits.
- **Workspace Integration**: Configured via `.worker.env` with `ADDITIONAL_DIRS="/home/kw/workspace/gem5"` to allow worker context access to reference simulators.

### 2. Strict Inter-AI English Protocol
- All tasks in `tasks.txt`, summaries in `progress.log`, status in `worker.status`, and git commits MUST be in **strictly English** to conserve tokens and maximize execution precision.

### 3. Dynamic Granularity Ladder (動態任務顆粒度)
Terminal A dynamically sizes tasks into `tasks.txt` based on `docs/agents/granularity-profile.json`:

| Level | Granularity | Scope & Action |
| :---: | :--- | :--- |
| **Level 1** | **Coarse (Seam / Feature)** | Specify public interface & verification test. Worker autonomously creates internal helpers & implementation. |
| **Level 2** | **Medium (Component Slice)** | Break down into discrete module units (e.g. data model vs processor). |
| **Level 3** | **Micro (Step / Function)** | Precise function-level instructions. Used only when worker requested guidance or previous attempt struggled. |

Task Format:
`TASK-XXX | LEVEL: 1 | TARGET: <files> | ACTION: <concrete logic> | VERIFY: <test command> | CONSTRAINTS: <bounds>`

### 4. Honest Worker Treaty & Escalation (誠實工人公約)
1. **Single-Seam Freedom (接縫自由實作)**: As long as a task operates on a single architectural module/seam, there is **NO line-of-code limit**. Worker has full autonomy on internal helpers, data structures, and tests.
2. **Cross-Subsystem Rejection (跨子系統過載拒絕)**: If a task demands simultaneous, uncoordinated changes across multiple decoupled subsystems, Worker emits `[NEED_DECOMPOSITION]` with proposed subtasks.
3. **3-Tier Escalation & Best-Effort with Note**:
   - *Tier 1 (Local choices)*: Worker decides autonomously.
   - *Tier 2 (Recoverable errors)*: Worker self-heals within 3 retries (with `git restore` clean intermediate resets).
   - *Tier 3 (Fatal blockers)*: Worker emits `[NEED_GUIDANCE]` or `[BLOCKED]` and pauses.
   - *Minor ambiguities*: Advance with Best-Effort and leave a non-blocking `Notes:` entry in `progress.log`.

### 5. Safe Git Log Rules for Acceptance AI
- Allowed: `git log -n 1 --stat`, `git show --stat <commit-hash>`, `git log --oneline -n 5`
- Prohibited: Bare `git log`, `git log -p`, full `git diff` (prevents context explosion).

### 6. Git Rebase Branch Integration
```bash
git checkout feature/<batch-name>
git rebase main
git checkout main
git merge --ff-only feature/<batch-name>
git branch -d feature/<batch-name>
```
