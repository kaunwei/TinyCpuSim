---
name: sentinel
description: 0-Token Queue Sentinel for streaming background worker supervision.
---

# Queue Sentinel (0-Token Supervision)

Supervise background Worker tasks without consuming LLM context tokens or polling loops.

## The Loop

Run the watcher in the background and end the turn. Never poll.

    WORKER_INACTIVE_REPEAT=1 bash nightshift/watch_worker.sh 0 5

Worker drains `tasks.txt` sequentially. Individual task completions (`[DONE]`, `[PASS]`) do **not** wake Planner.

## Wakeup Triggers

Host notifications wake Planner **only** on:
- `[SENTINEL_COMPLETED]`: Entire queue is empty (`IDLE`).
- `[BLOCKED]` / `PAUSED_FOR_PLANNER` / `[SCOPE_BLOCKED]`: Unrecoverable blocker or decomposition required.
- `[SENTINEL_INACTIVE]`: Stale heartbeat across periods (alert user and keep waiting).

## Anti-patterns
- **Per-task watching**: Passing `TASK-ID` during batch runs causes unnecessary Planner wakeups. Pass `TASK-ID` only for manual single-ticket diagnosis.
- **Heartbeat polling**: Querying `worker.status` in a chat loop burns context tokens. Trust the sentinel.
- **Timeout as failure**: Stale observer timeout is not proof of task failure—require task-scoped audit evidence.
