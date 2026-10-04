---
name: watch-worker-queue
description: "Deploy a lightweight Sentinel Subagent to monitor the Terminal B worker daemon and trigger proactive wakeup when all tasks are complete."
---

# Watch Worker Queue Skill (Sentinel Subagent Pattern)

This skill teaches the Interactive Architect AI (Terminal A) how to bridge external Terminal B background execution with proactive, event-driven in-session notifications using a **Sentinel Subagent**.

---

## 1. When to Use
Use this pattern whenever:
- Planner finishes decomposing a batch of tasks into `tasks.txt`.
- The human or workflow desires **proactive notification upon completion** (without human needing to manually ask "is it done?").
- Planner MUST NOT freeze its interactive terminal with `sleep` loops.

---

## 2. The Sentinel Workflow

```
[Planner] Writes batch tasks to tasks.txt
   │
   ├─► Calls invoke_subagent(Role="Queue Sentinel", TypeName="self" or "research")
   │
   ▼
[Planner yields immediately] ➔ Conversation remains interactive for the Human.
   │
   ▼
[Sentinel Subagent]
   • Runs: `bash scripts/watch_worker.sh 3600` (zero LLM token consumption while waiting)
   • When script exits 0 (Completed) or 1 (Blocked):
   • Reads: `head -n 15 progress.log`
   • Sends completion message back to Planner.
   │
   ▼
[System Event Wakeup] ➔ Planner wakes up automatically and delivers final acceptance report to Human.
```

---

## 3. Invocation Example

```json
{
  "Subagents": [
    {
      "TypeName": "self",
      "Role": "Queue Sentinel",
      "Prompt": "Run `bash scripts/watch_worker.sh 3600`. When it exits, read `head -n 15 progress.log` and report whether tasks completed successfully or if any task is blocked."
    }
  ]
}
```
