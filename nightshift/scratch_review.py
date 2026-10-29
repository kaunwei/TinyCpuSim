"""Worker-only scratch pressure monitor. Decisions and deletion belong to its ticket."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import time
import uuid

TAG = "[MAINTENANCE: SCRATCH]"
REPORT = "nightshift/.scratch/review/last-review.json"
REPORT_LIMIT = 1024


def is_review(task):
    header = str(task or "").split("|", 1)[0]
    return header.strip().startswith("SCRATCH-REVIEW-") and TAG in header


def safe_runtime(runtime):
    runtime = Path(runtime).absolute()
    for path in (runtime, runtime / ".scratch", runtime / ".scratch/review",
                 runtime / ".scratch/attempts", runtime / ".scratch/failed_tasks"):
        if path.is_symlink():
            raise ValueError(f"scratch monitor refuses symlink: {path}")
    return runtime


def inventory(runtime):
    """Stat files without opening logs or following symlinks."""
    runtime = safe_runtime(runtime)
    result = {"attempts_bytes": 0, "failed_bytes": 0, "failed_dirs": 0}
    for name, key in (("attempts", "attempts_bytes"), ("failed_tasks", "failed_bytes")):
        base = runtime / ".scratch" / name
        if not base.is_dir():
            continue
        if name == "failed_tasks":
            result["failed_dirs"] = sum(p.is_dir() and not p.is_symlink() for p in base.iterdir())
        for parent, directories, files in os.walk(base, followlinks=False):
            directories[:] = [d for d in directories if not (Path(parent) / d).is_symlink()]
            for name in files:
                path = Path(parent) / name
                try:
                    if not path.is_symlink():
                        result[key] += path.stat().st_size
                except FileNotFoundError:
                    pass  # A concurrent completed review may remove an old artifact.
    return result


@contextmanager
def monitor_lock(runtime):
    directory = safe_runtime(runtime) / ".scratch/review"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "monitor.lock"
    if path.is_symlink():
        raise ValueError("unsafe monitor lock")
    with path.open("a+b") as handle:
        if os.name == "nt":
            import msvcrt
            handle.seek(0, 2)
            if handle.tell() == 0:
                handle.write(b"0")
                handle.flush()
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(handle, fcntl.LOCK_EX)
        try:
            yield directory
        finally:
            if os.name == "nt":
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle, fcntl.LOCK_UN)


def read_small(path, limit=8192):
    if path.is_symlink():
        raise ValueError(f"unsafe state: {path}")
    if not path.exists():
        return {}
    with path.open("rb") as handle:
        raw = handle.read(limit + 1)
    if len(raw) > limit:
        raise ValueError(f"oversized state: {path}")
    return json.loads(raw)


def task_line(task_id):
    return (f"{task_id} | LEVEL: 1 | TARGET: nightshift/.scratch/attempts, "
            f"nightshift/.scratch/failed_tasks, {REPORT} | ACTION: {TAG} Review obsolete artifacts; "
            "delete only proven safe entries under the maintenance prompt | VERIFY: true")


def check(runtime, tasks, insert, write, now=None):
    if os.environ.get("SCRATCH_REVIEW_ENABLED", "1") == "0":
        return {"state": "disabled"}
    now = time.time() if now is None else now
    with monitor_lock(runtime) as directory:
        stats = inventory(runtime)
        state = read_small(directory / "state.json")
        queue = Path(tasks).read_text() if Path(tasks).exists() else ""
        pending = next((line for line in queue.splitlines() if is_review(line)), "")
        if pending:
            state.update(pending=pending, queued_at=now)
            write(directory / "state.json", json.dumps(state))
            return {"state": "pending", **stats}
        if state.get("pending"):
            # A claimed job is absent from the queue; PID + current_task protect it.
            for path in Path(runtime).glob("worker*.status"):
                status = read_small(path)
                if status.get("current_task") == state["pending"]:
                    try:
                        os.kill(int(status["pid"]), 0)
                        return {"state": "running", **stats}
                    except (OSError, ValueError, KeyError, TypeError):
                        pass
            if now - state.get("queued_at", now) < 120:
                return {"state": "pending", **stats}
            insert(tasks, state["pending"])
            state["queued_at"] = now
            write(directory / "state.json", json.dumps(state))
            return {"state": "requeued", **stats}
        limits = {"attempts_bytes": float(os.environ.get("SCRATCH_REVIEW_ATTEMPTS_MB", "100")) * 1048576,
                  "failed_bytes": float(os.environ.get("SCRATCH_REVIEW_FAILED_MB", "100")) * 1048576,
                  "failed_dirs": int(os.environ.get("SCRATCH_REVIEW_FAILED_DIRS", "20"))}
        if not any(limit > 0 and stats[key] >= limit for key, limit in limits.items()):
            return {"state": "below_threshold", **stats}
        previous = state.get("stats", {})
        growth = any(stats[key] >= previous.get(key, stats[key]) + max(5 if key == "failed_dirs" else 1048576,
                          previous.get(key, 0) // 4) for key in stats)
        if now - state.get("finished_at", 0) < 86400 and not growth:
            return {"state": "cooldown", **stats}
        task = task_line("SCRATCH-REVIEW-" + uuid.uuid4().hex[:12])
        # Persist intent first so a crash cannot lose the maintenance task.
        state.update(pending=task, queued_at=now, stats=stats)
        write(directory / "state.json", json.dumps(state))
        insert(tasks, task)
        return {"state": "queued", **stats}


def validate_report(runtime, task_id):
    report = read_small(Path(runtime) / ".scratch/review/last-review.json", REPORT_LIMIT)
    if report.get("task_id") != task_id or report.get("status") != "reviewed":
        raise ValueError("missing current maintenance report")
    if set(report) != {"task_id", "status", "before_bytes", "after_bytes", "removed", "kept", "note"}:
        raise ValueError("unexpected report fields")
    for key in ("before_bytes", "after_bytes"):
        if type(report[key]) is not int or report[key] < 0:
            raise ValueError("invalid byte count")
    removed = report["removed"]
    if (not isinstance(removed, list) or len(removed) > 5 or
        any(not isinstance(name, str) for name in removed) or len(set(removed)) != len(removed)):
        raise ValueError("at most five unique removed directories")
    for name in removed:
        parts = Path(name).parts
        if (not isinstance(name, str) or ".." in parts or task_id in parts or
            not ((parts[:3] == ("nightshift", ".scratch", "attempts") and len(parts) == 5) or
                 (parts[:3] == ("nightshift", ".scratch", "failed_tasks") and len(parts) == 4))):
            raise ValueError("removed path outside maintenance boundary")
        runtime = Path(runtime)
        candidate = runtime.joinpath(*parts[1:])
        if candidate.exists() or candidate.is_symlink():
            raise ValueError("reported removal still exists")
    if (not isinstance(report["kept"], dict) or len(report["kept"]) > 6 or
        any(not isinstance(k, str) or len(k) > 40 or type(v) is not int or v < 0
            for k, v in report["kept"].items()) or
        not isinstance(report["note"], str) or len(report["note"]) > 160):
        raise ValueError("report must contain only compact counts and note")
    return report


def finish(runtime, task_id, write, error="", attempt=""):
    with monitor_lock(runtime) as directory:
        if error:
            write(directory / "last-review.json", json.dumps({"task_id": task_id,
                  "status": "failed", "note": str(error)[:160]}, ensure_ascii=True))
        else:
            validate_report(runtime, task_id)
            if attempt:
                write(Path(attempt) / "maintenance-finished.json", json.dumps({"task_id": task_id, "finished": True}))
        state = read_small(directory / "state.json")
        state.update(pending="", finished_at=time.time(), stats=inventory(runtime))
        write(directory / "state.json", json.dumps(state))


def prompt(task, task_id):
    return f"""Execute Worker-only scratch maintenance. TASK_ID: {task_id}
{task}
This task has no Planner acceptance, score change, research report or product-code change.
Inspect at most 8 candidate directories and delete at most 5; complete with kept counts if uncertain.
The runtime creates attempts/<task>/<attempt>/baseline/snapshot.json plus adapter outputs;
rollback creates failed_tasks/<archive>/recovery.json plus diffs. They are diagnostic/recovery evidence.

DELETE BOUNDARY (ALL must hold):
- Only whole nightshift/.scratch/attempts/<task>/<attempt> or nightshift/.scratch/failed_tasks/<archive>.
- Every file in the candidate is at least 7 days old; never follow symlinks or delete a symlink tree.
- snapshot/recovery identity must match the task/attempt and current work_dir. Unknown/legacy evidence stays.
- Ordinary task: newest exact-task audit is PASS with Planner_Review: accept and matching tasks.done DONE;
  no subsequent rejection/failure, queued retry, active worker, dependency, rollback or report reference.
- Failed archive additionally needs recovered:true, matching baseline/attempt identity and finally accepted task.
- Preserve the newest attempt and newest failure archive per ordinary task as diagnostic anchors.
- Old internal SCRATCH-REVIEW attempts need maintenance-finished.json with finished:true and matching identity;
  preserve the latest internal maintenance attempt, current task, pending jobs and unresolved failures.
KEEP active/current/pending acceptance/retry/recovery, referenced evidence and anything you cannot prove safe.
Never modify queues, DONE, progress, status/PID, registry, score/events, code-index, product files or review state.
Only the fixed report below may be written outside the two deletion roots. Never stage or commit.

TOKEN BOUNDARY:
Use file size/mtime and small snapshot/recovery headers first. Do not dump raw logs or scan all history.
Search exact candidate task IDs only in queue/status/DONE and progress audit lines. Search reference paths
only in live reports/rollback metadata. If evidence needs broad reading, keep it as unknown.
Measure before/after with: python3 nightshift/worker_runtime.py scratch-review-check --root . --stats-only

Overwrite {REPORT}, UTF-8 JSON <=1024 bytes, exactly:
{{"task_id":"{task_id}","status":"reviewed","before_bytes":0,"after_bytes":0,
"removed":[],"kept":{{"unknown":0}},"note":"brief reason"}}
Byte counts are attempts_bytes + failed_bytes. removed contains <=5 exact project-relative directories;
kept contains <=6 short reason/count pairs; note <=160 characters. No report history or other report files.
Return exactly one JSON object: outcome=complete|blocked|failure, changed=boolean,
verification_evidence, commit_message, attempts=1, confidence=HIGH|MED|LOW. Keep unsafe entries and finish.
"""
