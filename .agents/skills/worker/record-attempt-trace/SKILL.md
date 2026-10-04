---
name: record-attempt-trace
description: "Guidelines for background worker to record transparent, honest execution attempts, retry traces, and confidence scores into progress.log."
---

# Record Attempt Trace Skill (Worker AI)

This skill teaches the background Worker AI how to truthfully record its execution steps, retry attempts, and self-assessed confidence into `progress.log` without hiding mistakes or guessing.

---

## The Honest & Concise Reporting Standard (誠實與極簡回報準則)

1. **Zero Concealment**: If a build failed on Attempt 1 because of an error or missing symbol, record it truthfully in the `Attempt Trace`.
2. **Dense & Strict Line Limit**:
   - `progress.log` block MUST be **<= 15 lines**.
   - No discursive explanations. Use dense, high-signal bullet points.
3. **Pointers Over Essays**: Point to files, diffs, or line ranges (`src/module.py#L12-L30`). Planner will inspect deeper on-demand.
4. **Confidence Metric**:
   - `HIGH`: Tests passed 100%, 0 warnings, code strictly follows standard conventions.
   - `MEDIUM`: Tests passed, but required 2-3 retries to get linker or test fixtures aligned.
   - `LOW`: Tests passed, but code felt brittle or edge cases remain unverified.
5. **Escalate Early**: If you detect requirement ambiguity or contradictory interfaces, DO NOT guess or invent fake data. Immediately emit `[NEED_GUIDANCE]` and exit cleanly.

---

## Log Template (Strictly <= 15 lines)

```text
================================================================================
[SUCCESS] TASK-XXX: <Task Title>
Time: <YYYY-MM-DD HH:MM:SS> | Duration: <Xs> | Attempts: <N>/3 | Confidence: <HIGH|MED|LOW>
Task Granularity: <Level 1|Level 2|Level 3>
Changes: <Modified files / Report path>
Verification: Build PASS, Tests PASS (<N> passed), GUI Smoke PASS
Attempt Trace:
  • Attempt 1: <PASS or failure reason with exact error summary>
  • Attempt 2: <Fix applied and result>
Granularity Feedback: <Worker observation on module granularity>
================================================================================
```
