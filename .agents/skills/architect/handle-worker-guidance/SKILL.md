---
name: handle-worker-guidance
description: "Handle worker [NEED_GUIDANCE] and [NEED_DECOMPOSITION] requests cleanly without breaking queue automation."
---

# Handle Worker Guidance Skill (Architect AI)

This skill teaches the Interactive Architect AI (Terminal A) how to process worker questions, ambiguity reports, or decomposition proposals without disrupting the automated pipeline.

---

## When to Use
Activate this skill when `progress.log` or `worker.status` indicates:
- `[NEED_GUIDANCE]`: Worker found an architectural ambiguity or conflicting specification.
- `[NEED_DECOMPOSITION]`: Worker evaluated a Level 1 task and determined it spans too many subsystems.

---

## Handling Workflow

### Case 1: Worker requested Guidance (`[NEED_GUIDANCE]`)
1. Read the exact question under `Question for Manager:` in `progress.log`.
2. Consult the human user in Traditional Chinese (繁體中文):
   > "Worker 在執行 TASK-XXX 時暫停求助，詢問：[問題摘要]。建議我們選擇 [選項A / 選項B]，您同意嗎？"
3. Upon human decision, formulate the clarification.
4. Prepend the updated task back to `tasks.txt` with a `GUIDANCE:` field:
   ```text
   TASK-XXX | LEVEL: 2 | TARGET: <files> | ACTION: <action> | GUIDANCE: <Resolution to the question> | VERIFY: <cmd> | CONSTRAINTS: <limits>
   ```

### Case 2: Worker requested Decomposition (`[NEED_DECOMPOSITION]`)
1. Read the worker's proposed subtask breakdown from `progress.log`.
2. Review the breakdown with the human user.
3. Replace the original oversized task in `tasks.txt` with the decomposed subtasks (tagged `LEVEL: 2` or `LEVEL: 3`).
4. Worker will automatically pop the first subtask within 5 seconds.
