---
name: calibrate-task-granularity
description: "Calibrate and decide task granularity (Level 1 Coarse, Level 2 Medium, Level 3 Micro) dynamically based on the module's granularity profile and past worker performance."
---

# Calibrate Task Granularity Skill (Architect AI)

This skill teaches the Interactive Architect AI (Terminal A) how to dynamically determine the optimal task granularity when decomposing human requirements into `tasks.txt`.

---

## The Granularity Ladder (顆粒度階梯)

| Level | Granularity | Scope & Description | When to Use |
| :--- | :--- | :--- | :--- |
| **Level 1** | **Coarse (Feature / Seam Level)** | Declares only public interface, behavior contract, and verification test. The worker autonomously creates internal structs, helper classes, and implementation files. | **Default choice for new tasks**, or when module performance score is 4-5 stars. |
| **Level 2** | **Medium (Component / Slice Level)** | Breaks feature down into discrete component units (e.g. data model vs processor vs serializer). Specifies file targets and key class signatures. | When module performance is 3 stars, or worker had 1-2 retries on past attempts. |
| **Level 3** | **Micro (Step / Function Level)** | Highly detailed function signatures, exact logic steps, and minimal diff scopes. | When module performance is 1-2 stars, worker failed/blocked previously, or human explicitly requests step-by-step guidance. |

---

## Dynamic Sizing Workflow

1. **Check Module Profile**: Read `docs/agents/granularity-profile.json` to check the current rating of the target subsystem.
2. **Determine Level**:
   - If profile says `Level 1` or module is unrated ➔ **Generate Level 1 (Coarse)** task.
   - If profile says `Level 2` ➔ **Generate Level 2 (Medium)** tasks.
   - If profile says `Level 3` ➔ **Generate Level 3 (Micro)** tasks.
3. **Format Task Line for `tasks.txt`**:
   Always tag the level in the task line:
   ```text
   TASK-001 | LEVEL: 1 | TARGET: src/calc.py, tests/test_calc.py | ACTION: Implement Calculator engine with add, subtract, multiply, divide and zero division handling | VERIFY: pytest tests/test_calc.py | CONSTRAINTS: Pure Python stdlib, zero warnings
   ```
4. **Post-Acceptance Adjustment**:
   After reading the worker's audit trace in `progress.log`, invoke `/score-worker-performance` to raise or lower the level for subsequent tasks.
