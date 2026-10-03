---
name: score-worker-performance
description: "Score worker execution performance (1 to 5 stars) from progress.log audit traces and update docs/agents/granularity-profile.json automatically."
---

# Score Worker Performance Skill (Architect AI)

This skill teaches the Interactive Architect AI (Terminal A) how to read the worker's execution and retry audit trace in `progress.log`, score the performance (1 to 5 stars), and update the module's capability memory in `docs/agents/granularity-profile.json`.

---

## Scoring Rubric (評分標準)

| Score | Rating | Criteria | Granularity Adjustment |
| :---: | :--- | :--- | :--- |
| ⭐⭐⭐⭐⭐ | **5 Stars (Mastery)** | Passed in 1 attempt (0 retries), 0 compiler/lint warnings, clean diff. | **Promote / Maintain Level 1 (Coarse)**. Worker is fully autonomous. |
| ⭐⭐⭐⭐ | **4 Stars (Proficient)** | Passed in 2 attempts (1 retry fixed autonomously), all tests green. | **Maintain current Level (1 or 2)**. |
| ⭐⭐⭐ | **3 Stars (Struggling)** | Passed on 3rd attempt (2 retries needed), or minor confusion. | **Demote to Level 2 (Medium)** for next tasks in this subsystem. |
| ⭐⭐ | **2 Stars (Need Help)** | Worker raised `[NEED_GUIDANCE]` or `[NEED_DECOMPOSITION]` honestly. | **Demote to Level 2/3**, provide clarification via `/handle-worker-guidance`. |
| ⭐ | **1 Star (Blocked/Timeout)** | Exceeded 3 retries, timed out, or crashed. | **Demote to Level 3 (Micro)**, re-architect and split task with user. |

---

## Process

1. **Read `progress.log` Top Entry**:
   Inspect `Attempts`, `Attempt Trace`, `Duration`, and `Confidence`.
2. **Assign Score**: Match against the rubric above.
3. **Update `docs/agents/granularity-profile.json`**:
   Update the subsystem entry:
   ```json
   {
     "modules": {
       "src/calc": {
         "recommended_level": "Level 1 (Coarse)",
         "last_score": 5,
         "success_rate": "100%",
         "total_tasks": 1,
         "notes": "Handles arithmetic and error boundary tests cleanly in 1 attempt"
       }
     }
   }
   ```
4. **Brief the User**: Report score and adjustment to the human in 1-2 lines.
