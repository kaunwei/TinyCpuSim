# NightShift Project Porting & Deployment Guide

> **Porting Status**: [x] Completed (TinyCpuSim CTest Hot/Cold Dual-Phase Acceptance Configured)
>
> Once marked completed (or when `nightshift/.ported` exists), `install.sh` will never overwrite your project-specific configurations or this porting guide during future updates.

---

## 1. Overview & Architecture

NightShift provides a 24-hour unattended dual-loop AI worker daemon. The daemon continuously consumes development tasks from `nightshift/tasks.txt`, executes them with registered agent adapters (Codex, Antigravity, etc.), and enforces rigorous test-driven acceptance before committing changes to Git.

### Dual-Phase Acceptance Model: Hot vs. Cold

```
                    +---------------------------+
                    |  Next Task in tasks.txt   |
                    +-------------+-------------+
                                  |
                                  v
                    +---------------------------+
                    | Agent Execution (TDD)     |
                    +-------------+-------------+
                                  |
                                  v
                    +---------------------------+
                    | [HOT ACCEPTANCE]          |  <- Fast: tests touched modules & new TDD cases
                    +-------------+-------------+
                                  | Pass
                                  v
                    +---------------------------+
                    | Git Commit & Streak Score |
                    +-------------+-------------+
                                  |
               +------------------+------------------+
               | Every cold_every tasks (e.g. 5)     | Other tasks
               v                                     v
+-------------------------------+      +---------------------------+
| [COLD ACCEPTANCE]             |      | Wait for next task        |
| Full regression suite         |      +---------------------------+
| Clean tree (git clean -fd)    |
+--------------+----------------+
               | Pass
               v
+-------------------------------+
| Silent pass; ready next task  |
+-------------------------------+
```

1. **Hot Acceptance (`acceptance.hot`)**:
   - Executes immediately after each development task finishes and before changes are committed.
   - Fast, focused gate: verifies the new TDD case or touched module without waiting for slow project-wide test suites.
   - Ensures that every committed task introduces passing, verified test evidence.
2. **Cold Acceptance (`acceptance.cold`)**:
   - Runs periodically after every `cold_every` successfully committed development tasks (e.g., every 5 tasks), or on-demand via a `[COLD]` task.
   - Complete project regression suite run in a clean workspace (`git reset --hard HEAD && git clean -fd`).
   - Ensures long-term project stability, catches hidden integration regressions, and requires a strictly clean working tree (no untracked non-ignored artifacts).

---

## 2. Directory Layout & Ownership

Installing NightShift into a project creates the self-contained `nightshift/` directory:

| Path | Purpose |
| --- | --- |
| `nightshift/config.json` | **Primary configuration**: acceptance gates (`hot`, `cold`), agent registry, and code index scope. |
| `nightshift/PORTING_GUIDE.md` | This porting guide and project porting checklist. |
| `nightshift/worker.sh` | Main daemon launcher script (`./nightshift/worker.sh` or `RUN_ONCE=1 ./nightshift/worker.sh`). |
| `nightshift/worker_runtime.py` | Daemon runtime engine and CLI commands. |
| `nightshift/lifecycle.py` | Task dispatch, state transitions, pause handling, and error recovery. |
| `nightshift/acceptance.py` | Acceptance engine executing hot and cold test runners. |
| `nightshift/unittest_hot.py` | Built-in Python unittest AST fingerprinting and hot discovery runner. |
| `nightshift/tasks.txt` | FIFO task queue (one task description per non-comment line). |
| `nightshift/tasks.done` | Completed task history with execution timestamps. |
| `nightshift/progress.log` | Human-readable audit log of daemon progress and task outcomes. |
| `nightshift/worker.status` | Machine-readable worker status (`IDLE`, `RUNNING`, `PAUSED`, `GATE_FAILED`, etc.). |
| `nightshift/code-index.toml` | Tree-sitter index scope configuration (project vs. tool boundaries). |
| `nightshift/adapters/` | Protocol adapters for Codex, AgY, and generic JSON agents. |
| `nightshift/tools/` | Multi-language Tree-sitter indexing tool with private virtualenv. |
| `nightshift/.scratch/` | Runtime scratch space: attempt snapshots, code index database, pressure reviews. |
| `.agents/skills/` | Agent skills installed to host discovery paths (Planner, Explore, Universal Build/Verify). |

---

## 3. Configuration Contract: `nightshift/config.json`

All runtime settings live in `nightshift/config.json`. Below is the complete schema with default settings:

```json
{
  "version": 1,
  "acceptance": {
    "hot": "python3 nightshift/unittest_hot.py --start test",
    "cold": "python3 -m unittest discover -s test",
    "cold_every": 5,
    "timeout": 180,
    "wiring": []
  },
  "agents": {
    "codex-main": {
      "default": true,
      "worker": {
        "step_base": 22,
        "step_per_file": 5,
        "task_timeout_seconds": 1800,
        "research_step_budget": 100
      },
      "agent": {
        "adapter": "codex",
        "command": "codex",
        "args": [],
        "env": {},
        "config": {
          "model": "gpt-6-luna",
          "effort": "low",
          "approval_mode": "auto",
          "sandbox": "workspace-write",
          "ephemeral": "1"
        }
      }
    },
    "agy-backup": {
      "default": false,
      "worker": {
        "task_timeout_seconds": 1800,
        "research_step_budget": 100
      },
      "agent": {
        "adapter": "agy",
        "command": "agy",
        "args": [],
        "env": {},
        "config": {
          "model": "gemini-3.8-flash-low",
          "effort": "low",
          "mode": "accept-edits"
        }
      }
    }
  },
  "code_index": {
    "include": ["**/*"],
    "exclude": [],
    "tool": ["nightshift/tools/**"],
    "project": ["**/*"]
  }
}
```

### Acceptance Fields:

- **`hot`** (string, required): Shell command executed for hot acceptance from the project root. Exit code 0 indicates success.
- **`cold`** (string, required): Shell command executed for cold regression from the project root. Exit code 0 indicates success.
- **`cold_every`** (integer, optional): Frequency of automatic cold regression runs. If set to `5`, cold runs after every 5 successfully committed development tasks. Set to `0` or omit to run cold only when explicitly requested via queue.
- **`timeout`** (integer, default `180`): Maximum execution time in seconds for acceptance commands before aborting.
- **`wiring`** (array of strings, default `[]`): Explicit allowlist of configuration or registration files that Ghost agent is permitted to edit when connecting newly created tests.

---

## 4. Adapting Hot & Cold for Your Project

Different languages and test frameworks have different commands. Configure `acceptance.hot` and `acceptance.cold` in `nightshift/config.json` according to your project stack:

### A. Python Projects

- **Standard `unittest` (Default)**:
  ```json
  "acceptance": {
    "hot": "python3 nightshift/unittest_hot.py --start test",
    "cold": "python3 -m unittest discover -s test",
    "cold_every": 5
  }
  ```
- **`pytest`**:
  ```json
  "acceptance": {
    "hot": "pytest -m 'not slow' -q",
    "cold": "pytest",
    "cold_every": 5
  }
  ```

### B. C / C++ Projects (TinyCpuSim Configuration)

TinyCpuSim utilizes dynamic CTest test partitioning managed by `scripts/nightshift_acceptance.py`:
- **`hot` Acceptance**: The most recent 100 test cases (with any boundary test suite spanning the 100-test cutoff kept together without being arbitrarily split, e.g. `lsu_ubench_test`).
- **`cold` Acceptance**: Full regression across all remaining test cases, ensuring zero regression across the codebase.
- **Dynamic & Extensible**: As new test cases are added, hot automatically adapts. To explicitly pin or add cases/patterns, run `/usr/bin/python3 scripts/nightshift_acceptance.py --add-hot <pattern>` or edit `nightshift/acceptance_config.json`.

Configuration in `nightshift/config.json`:
```json
"acceptance": {
  "hot": "/usr/bin/python3 scripts/nightshift_acceptance.py --suite hot",
  "cold": "/usr/bin/python3 scripts/nightshift_acceptance.py --suite cold",
  "cold_every": 5,
  "timeout": 180,
  "wiring": [
    "nightshift/acceptance_config.json"
  ]
}
```

CLI Utilities:
- `./scripts/nightshift_acceptance.py --list`: Inspect current hot/cold counts and test boundary.
- `./scripts/nightshift_acceptance.py --add-hot <pattern>`: Add pattern or test to hot.
- `./scripts/nightshift_acceptance.py --add-cold <pattern>`: Add pattern or test to cold.
- `./scripts/02_run_tests.sh --hot` / `--cold`: Run directly via regression runner script.

### C. Rust Projects (Cargo)

```json
"acceptance": {
  "hot": "cargo test --lib --quiet",
  "cold": "cargo test --all",
  "cold_every": 5
}
```

### D. Go Projects

```json
"acceptance": {
  "hot": "go test -short ./...",
  "cold": "go test -v ./...",
  "cold_every": 5
}
```

### E. JavaScript / TypeScript Projects (Vitest / Jest / npm)

- **Vitest**:
  ```json
  "acceptance": {
    "hot": "npx vitest run --changed",
    "cold": "npx vitest run",
    "cold_every": 5
  }
  ```
- **Jest**:
  ```json
  "acceptance": {
    "hot": "npx jest -o --passWithNoTests",
    "cold": "npx jest --ci",
    "cold_every": 5
  }
  ```

---

## 5. Advanced Hot Runner Customization

By default, NightShift executes `hot` and `cold` via bash (`acceptance.DefaultAcceptanceRunner`). When the hot command runs, NightShift provides environment variables:

| Environment Variable | Description |
| --- | --- |
| `NIGHTSHIFT_PHASE` | Current phase: `"discover"` or `"run"`. |
| `NIGHTSHIFT_RESULT` | Path where the runner must write structured JSON result (if providing fine-grained case tracking). |
| `NIGHTSHIFT_RUN` | Unique nonce to be echoed in the result output. |
| `NIGHTSHIFT_BASE` | Commit SHA at task start for diff / AST baseline comparison. |
| `NIGHTSHIFT_TASK` | Active Task ID (e.g., `"TASK-001"`). |
| `NIGHTSHIFT_REQUIRED` | Required case IDs passed as JSON array string. |

### Implementing a Custom Runner: `nightshift/acceptance_runner.py`

If your project requires custom Python test discovery or custom test harness orchestration, create `nightshift/acceptance_runner.py` in your project root:

```python
import acceptance

class ProjectRunner(acceptance.DefaultAcceptanceRunner):
    def run_hot(self, args, result, run_verify):
        # Custom setup, container execution, or targeted test discovery
        return super().run_hot(args, result, run_verify)

    def run_cold(self, root, run_verify):
        # Custom full-suite execution or integration environment teardown
        return super().run_cold(root, run_verify)

def get_runner(root, cfg):
    return ProjectRunner()
```

---

## 6. Porting Checklist & Smoke Test

Follow this step-by-step checklist to verify your porting:

- [x] **Step 1: Install NightShift**
  ```bash
  ./install.sh /path/to/project
  cd /path/to/project
  ```
- [x] **Step 2: Configure `nightshift/config.json`**
  - Set `acceptance.hot` to your project's fast/unit test command.
  - Set `acceptance.cold` to your project's full test suite command.
  - Verify `agents` executable commands (e.g. `codex`, `agy`) are installed and in `$PATH`.
- [x] **Step 3: Test Hot Acceptance Manually**
  Run your hot acceptance command directly in the project root:
  ```bash
  eval "$(python3 -c 'import json; print(json.load(open("nightshift/config.json"))["acceptance"]["hot"])')"
  ```
  Ensure it exits 0.
- [x] **Step 4: Test Cold Acceptance Manually**
  Run your cold acceptance command directly in the project root:
  ```bash
  eval "$(python3 -c 'import json; print(json.load(open("nightshift/config.json"))["acceptance"]["cold"])')"
  ```
  Ensure it exits 0 and leaves no untracked files (`git status --porcelain` is clean).
- [x] **Step 5: Build Code Index**
  ```bash
  python3 nightshift/worker_runtime.py code-index build
  python3 nightshift/worker_runtime.py code-index query symbol main --scope project
  ```
- [x] **Step 6: Run a Single Task Trial**
  Add a test task to `nightshift/tasks.txt`:
  ```bash
  echo "Add a minimal test case to verify worker pipeline" >> nightshift/tasks.txt
  RUN_ONCE=1 ./nightshift/worker.sh
  ```
  Verify the worker completes the task, passes hot acceptance, commits to git, and updates `nightshift/progress.log`.
- [x] **Step 7: Mark Porting as Completed**
  Update the header of this file:
  ```markdown
  > **Porting Status**: [x] Completed
  ```
  *(Or execute `touch nightshift/.ported`)*.
  Once completed, `install.sh` will preserve your configuration and porting guide permanently.

---

## 7. Operating the Worker

From your project root:

```bash
# Start 24-hour unattended worker daemon:
./nightshift/worker.sh

# Run exactly one task and stop:
RUN_ONCE=1 ./nightshift/worker.sh

# Inspect current status:
./nightshift/status.sh

# Watch progress in real-time:
./nightshift/watch_worker.sh

# Pause future task claims safely (active task completes first):
touch nightshift/PAUSE

# Resume daemon:
rm -f nightshift/PAUSE
```
