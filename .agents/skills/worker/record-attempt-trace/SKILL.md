---
name: record-attempt-trace
description: "Guidelines for background worker to record transparent, honest execution attempts, retry traces, and confidence scores into progress.log."
---

# Record Attempt Trace Skill (Worker AI)

This skill teaches the background Worker AI how to truthfully record its execution steps, retry attempts, and self-assessed confidence into `progress.log` without hiding mistakes or guessing.

---

## The Honest Reporting Standard (誠實回報準則)

1. **Zero Concealment**: If a build failed on Attempt 1 because you forgot an include or misspelled a symbol, record it honestly in the `Attempt Trace`.
2. **Confidence Metric**:
   - `HIGH`: Tests passed 100%, 0 warnings, code strictly follows standard conventions.
   - `MEDIUM`: Tests passed, but required 2-3 retries to get linker or test fixtures aligned.
   - `LOW`: Tests passed, but code felt brittle or edge cases remain unverified.
3. **Escalate Early**: If you detect requirement ambiguity or contradictory interfaces, DO NOT guess or invent fake data. Immediately emit `[NEED_GUIDANCE]` and exit cleanly.

---

## Log Template

```text
================================================================================
[SUCCESS] TASK-XXX: <Task Title>
Time: <YYYY-MM-DD HH:MM:SS> | Duration: <Xs> | Attempts: <N>/3 | Confidence: <HIGH|MEDIUM|LOW>
Task Granularity: <Level 1|Level 2|Level 3>

Attempt Trace:
  • Attempt 1: <PASS or failure reason with exact error summary>
  • Attempt 2: <Fix applied and result>

Granularity Feedback: 
  <Worker observation on whether this subsystem is comfortable at Level 1 or needs Level 2>
================================================================================
```
