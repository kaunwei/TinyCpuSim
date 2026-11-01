# Hot/cold migration

Runtime commits and scores on the current branch. Planner schedules work and
explicitly restarts after failures. Setup is required; legacy VERIFY alone is
not evidence that a new TDD case ran. Generated lifecycle state lives under
`git rev-parse --git-path nightshift`; it is outside Git history and cleanup.

## Configure a project

Track `nightshift/config.json` with the project's acceptance and agent commands:

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
    "primary": {
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
        "config": {}
      }
    }
  }
}
```

Legacy standalone `nightshift/acceptance.json` and `nightshift/adapters/agents.json` remain supported for backward compatibility.

Commands run from project root. `cold_every` is optional: absent means no periodic
insertion; a manually queued cold still requires `cold`. Set a positive integer
to insert after that many successfully committed development tasks. Cold itself,
research, failed and no-change tasks do not advance cadence. Cold runs serially,
coalesces pending requests and skips an unchanged already-passing HEAD. Success
only updates internal state: no message, Worker score or Git record. Outputs must
be ignored; tracked/nonignored product mutations fail the cold task.

`wiring` lists exact configuration/registration files Ghost may edit to connect
existing tests. Include those files and tests in task TARGET. Ghost also may supply
the acceptance configuration. Keep executable test behavior in tests, separate
from wiring. Ghost leaves source, assertions and unrelated scripts unchanged.

### Custom Hot/Cold Runner Abstraction

By default, NightShift executes `hot` and `cold` commands via bash with `NIGHTSHIFT_PHASE` environment variables (`DefaultAcceptanceRunner`). A project can customize or completely override how hot and cold call functions work:

1. **Via `nightshift/acceptance_runner.py`**:
   Create `nightshift/acceptance_runner.py` in the project root, defining a custom `Runner` class (or function `get_runner(root, cfg)`) subclassing `acceptance.DefaultAcceptanceRunner` or `acceptance.AcceptanceRunner`:
   ```python
   import acceptance

   class CustomRunner(acceptance.DefaultAcceptanceRunner):
       def run_hot(self, args, result, run_verify):
           # Custom hot execution logic
           return super().run_hot(args, result, run_verify)

       def run_cold(self, root, run_verify):
           # Custom cold regression logic
           return super().run_cold(root, run_verify)

   def get_runner(root, cfg):
       return CustomRunner()
   ```

2. **Via `runner` in `nightshift/acceptance.json`**:
   Specify a custom runner entry point: `"runner": "my_module:CustomRunner"`.

Keep generated queues/status/scores untracked. Installer ignores them; if an old
project tracks these files, untrack its generated state before starting Worker.
Upgrade preserves project configuration; it does not invent a hot gate for you.

## Worker handoff

Use persistent framework discovery. A task can declare additional `CASES: id,id`
and `VERIFY: command`; both join hot acceptance. New TDD cases must remain part
of normal hot without these task selectors on later tasks.

```json
{
  "outcome": "complete",
  "commit_message": "fix: handle empty input",
  "tdd_cases": [
    {"id": "test_input.InputTests.test_empty", "source": "test/test_input.py"}
  ]
}
```

Missing declarations, unchanged test content, comment-only edits, ticket-only
cases and skipped/deselected results fail. Runtime may stage allowed changes
before hot, then commits only after the common gate passes. Worker runs TDD during
implementation; repeated post-commit VERIFY and hard-coded framework checks are gone.

## Framework adapter protocol

The hot command receives these environment variables:

| Variable | Meaning |
| --- | --- |
| NIGHTSHIFT_PHASE | discover or run |
| NIGHTSHIFT_RESULT | Unique JSON output path owned by this runtime invocation |
| NIGHTSHIFT_RUN | Nonce echoed by the output |
| NIGHTSHIFT_BASE | Task-start commit for test-content comparison |
| NIGHTSHIFT_TASK | Task ID; empty during permanent discovery |
| NIGHTSHIFT_REQUIRED | Required case IDs as JSON; empty during discovery |

Write this shape to NIGHTSHIFT_RESULT and exit zero only for success:

```json
{
  "run": "the received nonce",
  "cases": [{
    "id": "test_input.InputTests.test_empty",
    "source": "test/test_input.py",
    "fingerprint": "64 lowercase hex digits",
    "baseline_fingerprint": null,
    "status": "passed"
  }]
}
```

Discovery omits status. Fingerprints hash parsed test content (not comments,
docstrings or the whole test file); baseline_fingerprint is that same case's
fingerprint at NIGHTSHIFT_BASE, or null for a new case. Report stable IDs and
source associations. Execution reports actual case outcomes; absent, skipped,
failed or stale evidence fails. Every configured command must also exit zero.
Runtime invokes the adapter itself and requires fresh invocation identity.

The supplied unittest adapter supports top-level TestCase classes with methods
defined in the same file. It discovers real tests, compares method ASTs with Git
baseline and records actual unittest success. Import/discovery errors fail. A
framework with generated/inherited cases needs an adapter that can establish
equivalent source association; unsupported evidence fails explicitly. Source
change plus execution does not prove a test's assertions match the requirement.

For Swift/pytest/other frameworks, port discovery, source mapping and actual
result parsing to this contract. A bare `swift test`/pytest exit code is not a
replacement adapter. Run the new-case/persistent-discovery/skip/stale-evidence
fixtures before enabling a new integration. Framework-wide policy belongs here,
not in runtime heuristics.

## Failure and restart

1. Gate failure stops ordinary dispatch. Ghost gets actual output and may supply
   metadata, wire an existing test, rerun checks and write diagnosis. It has one
   bounded allowance per attempt; product/test repair goes to Planner.
2. Unresolved failure produces a report under the Git directory's
   `nightshift/reports` and notifies Planner. Ghost timeout still supplies available
   failure evidence. The dirty/staged scene remains available for inspection.
3. Planner inserts/reorders ordinary issue tasks through the queue scripts,
   revises instructions and explicitly runs:

       python3 nightshift/worker_runtime.py restart --root .

4. Next Worker starts after `reset --hard HEAD` and `clean -fd`: tracked and
   nonignored uncommitted changes are discarded. Ignored files/reports survive.
   No failed patch is stashed, archived or continued. Configured PAUSE and agent
   event gates remain separate controls. Cleanup failure keeps dispatch paused.

Cold failure uses the same diagnosis and restart flow, treated as a new issue;
earlier scores and commits remain. Successful development tasks use the existing
accept streak/score arithmetic automatically. Durable task/commit/event identities
make replay safe across interrupted commit, scoring and queue insertion.

## Check a migration

Use a disposable installed project. Add a real TDD case with Worker handoff,
observe hot/commit/score, then deliver another task and prove the first case still
runs without its old ticket. Set cold_every to two, observe a silent independent
cold, then fail cold and verify report/pause. Schedule an ordinary issue, restart
and confirm the next Worker sees clean tracked/nonignored files.

Repository verification: `python3 -m unittest discover -s test -p test_automatic_acceptance.py`.
These fixtures use real unittest hooks and disposable Git repositories, never a
model or cleanup of the live checkout.
