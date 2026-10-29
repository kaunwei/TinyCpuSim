#!/usr/bin/env python3
"""Portable, testable operations behind the nightshift/worker.sh entry point.

Worker settings come from the launcher environment; agent registrations are read
from nightshift/adapters/agents.json.
Each command receives explicit inputs so callers and tests exercise the same seam.
"""

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
from functools import wraps
import hashlib
import json
import math
import os
import platform
from pathlib import Path
import queue
import re
import shutil
import signal
import sys
import subprocess
import tempfile
import time
import threading
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scratch_review


FIELD_NAMES = (
    "LEVEL", "TARGET", "ACTION", "GOAL", "VERIFY",
    "FORBIDDEN_SHORTCUTS", "CONSTRAINTS", "DEPENDS_ON",
)


_workspace_state_lock = threading.RLock()


def serialize_workspace_state(function):
    """Keep heartbeat's atomic-write temporaries out of task snapshots."""
    @wraps(function)
    def serialized(*args, **kwargs):
        with _workspace_state_lock:
            return function(*args, **kwargs)
    return serialized


def atomic_write(path, content):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    old_mode = path.stat().st_mode & 0o777 if path.exists() else None
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="", dir=path.parent,
            prefix=f".{path.name}.", delete=False,
        ) as output:
            temporary = Path(output.name)
            output.write(content)
        if old_mode is not None:
            temporary.chmod(old_mode)
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def prepend_progress(path, entry):
    path = Path(path)
    lock = Path(f"{path}.lock")
    for _ in range(100):
        try:
            lock.mkdir()
            break
        except FileExistsError:
            time.sleep(0.05)
    else:
        raise TimeoutError(f"progress lock remains held: {lock}")
    try:
        history = path.read_text(encoding="utf-8") if path.exists() else ""
        atomic_write(path, f"{entry}\n{history}")
    finally:
        lock.rmdir()


def update_planner_review(progress_path, task_id, audit_ns, result):
    if result not in ("accept", "reject", "no_fault"):
        raise ValueError(f"unsupported Planner review result: {result}")
    path = Path(progress_path)
    lock = Path(f"{path}.lock")
    for _ in range(100):
        try:
            lock.mkdir()
            break
        except FileExistsError:
            time.sleep(0.05)
    else:
        raise TimeoutError(f"progress lock remains held: {lock}")
    try:
        lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
        prefix = "[MECHANICAL_AUDIT] Status: PASS |"
        matches = [index for index, line in enumerate(lines)
                   if line.startswith(prefix) and f"Task: {task_id} |" in line
                   and f"Audit_NS: {audit_ns}" in line]
        if len(matches) != 1:
            raise ValueError(f"expected one matching audit record, found {len(matches)}")
        index = matches[0]
        line = lines[index]
        if "Planner_Review:" not in line:
            raise ValueError("legacy audit requires confirmation before review")
        current = re.search(r"Planner_Review: ([a-z_]+)", line)
        if not current:
            raise ValueError("invalid Planner_Review field")
        if current.group(1) == result:
            return False
        if current.group(1) != "pending":
            raise ValueError(f"Planner review conflict: already {current.group(1)}")
        lines[index] = re.sub(r"Planner_Review: pending", f"Planner_Review: {result}", line, count=1)
        atomic_write(path, "".join(lines))
        return True
    finally:
        lock.rmdir()


def read_score_state(path):
    path = Path(path)
    if not path.exists():
        state = {"version": 1, "score": 5, "success_window": [], "failures": {}, "feedback": []}
        atomic_write(path, json.dumps(state, indent=2) + "\n")
        return state
    state = json.loads(path.read_text(encoding="utf-8"))
    if (state.get("version") != 1 or not isinstance(state.get("score"), int)
            or not 1 <= state["score"] <= 5
            or not isinstance(state.get("success_window"), list)
            or not isinstance(state.get("failures"), dict)):
        raise ValueError(f"invalid worker score state file: {path}")
    state.setdefault("feedback", [])
    return state


def score_task_level(score):
    return 1 if score == 5 else 2 if score >= 3 else 3


def score_summary(state):
    return {"score": state["score"], "task_level": score_task_level(state["score"]),
            "successes_in_window": len(state["success_window"])}


def registered_agent_score_path(root, registration_id):
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", registration_id).strip("._-") or "agent"
    digest = hashlib.sha256(registration_id.encode("utf-8")).hexdigest()[:12]
    return Path(root) / f"{safe[:48]}-{digest}.json"


def initialize_agent_score(root, registration_id, initial_score):
    if not isinstance(initial_score, int) or not 1 <= initial_score <= 5:
        raise ValueError("adapter get_config initial_score must be an integer from 1 through 5")
    path = registered_agent_score_path(root, registration_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    lock = Path(f"{path}.lock")
    for _ in range(100):
        try:
            lock.mkdir()
            break
        except FileExistsError:
            time.sleep(0.05)
    else:
        raise TimeoutError(f"agent score lock remains held: {lock}")
    try:
        if path.exists():
            state = read_score_state(path)
            if state.get("registration_id") != registration_id:
                raise ValueError(f"agent score identity mismatch: {path}")
            return state
        state = {"version": 1, "registration_id": registration_id,
                 "score": initial_score, "success_window": [],
                 "failures": {}, "feedback": []}
        atomic_write(path, json.dumps(state, ensure_ascii=False, indent=2) + "\n")
        return state
    finally:
        lock.rmdir()


def read_agent_score_state(root, registration_id):
    path = registered_agent_score_path(root, registration_id)
    if not path.is_file():
        raise ValueError(f"agent score has not been initialized by get_config: {registration_id}")
    state = read_score_state(path)
    if state.get("registration_id") != registration_id:
        raise ValueError(f"agent score identity mismatch: {registration_id}")
    return state


def append_agent_event(path, event, gate_path=""):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    lock = Path(f"{path}.lock")
    for _ in range(100):
        try:
            lock.mkdir()
            break
        except FileExistsError:
            time.sleep(0.05)
    else:
        raise TimeoutError(f"agent event lock remains held: {lock}")
    try:
        with path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(event, ensure_ascii=False) + "\n")
    finally:
        lock.rmdir()
    gate_path = Path(gate_path) if gate_path else path.parent / ".worker-dispatch-gate.json"
    gate_lock = Path(f"{gate_path}.lock")
    for _ in range(100):
        try:
            gate_lock.mkdir()
            break
        except FileExistsError:
            time.sleep(0.05)
    else:
        raise TimeoutError(f"dispatch gate lock remains held: {gate_lock}")
    try:
        gate_state = json.loads(gate_path.read_text(encoding="utf-8")) if gate_path.exists() else {
            "version": 1, "pending_event_ids": []}
        pending = gate_state.setdefault("pending_event_ids", [])
        if event["event_id"] not in pending:
            pending.append(event["event_id"])
        atomic_write(gate_path, json.dumps(gate_state, ensure_ascii=False, indent=2) + "\n")
    finally:
        gate_lock.rmdir()


def _read_json_lines(path):
    source = Path(path)
    if not source.is_file():
        return []
    rows = []
    for line_number, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as error:
            raise ValueError(f"invalid JSON line {line_number} in {source}: {error}") from error
        if not isinstance(row, dict):
            raise ValueError(f"event line {line_number} in {source} must be an object")
        rows.append(row)
    return rows


def format_planner_review_line(event):
    event_id = event.get("event_id", "unknown")
    return (f"[PLANNER_REVIEW_REQUIRED] Event={event_id} Cause={event.get('cause', 'unknown')} "
            f"Task={event.get('task_id', 'unknown')} Registration={event.get('registration_id', 'unknown')} "
            f"Score={event.get('previous_score', '?')}->{event.get('current_score', '?')} "
            f"Level={event.get('previous_level', '?')}->{event.get('current_level', '?')}; "
            "review affected queued work and acknowledge or revise it before dispatch resumes.")


def _watch_agent_events_locked(events_path, cursor_path, acknowledgements_path, gate_path, handoff_path, replay_pending=False):
    events = _read_json_lines(events_path)
    acknowledgements = _read_json_lines(acknowledgements_path)
    acked = {item.get("event_id") for item in acknowledgements}
    cursor_path, gate_path, handoff_path = map(Path, (cursor_path, gate_path, handoff_path))
    cursor = json.loads(cursor_path.read_text(encoding="utf-8")) if cursor_path.exists() else {
        "version": 1, "seen_event_ids": []}
    if cursor.get("version") != 1 or not isinstance(cursor.get("seen_event_ids"), list):
        raise ValueError(f"invalid agent event cursor: {cursor_path}")
    seen = set(cursor["seen_event_ids"])
    already_handed_off = handoff_path.read_text(encoding="utf-8") if handoff_path.exists() else ""
    emitted = []
    for event in events:
        event_id = event.get("event_id")
        if not isinstance(event_id, str) or not event_id:
            raise ValueError("agent event requires a non-empty event_id")
        line = format_planner_review_line(event)
        if event_id not in seen:
            if event_id not in already_handed_off:
                with handoff_path.open("a", encoding="utf-8") as stream:
                    stream.write(line + "\n")
                already_handed_off += line + "\n"
                if not replay_pending:
                    emitted.append(line)
            seen.add(event_id)
        if replay_pending and event_id not in acked:
            emitted.append(line)
    cursor_path.parent.mkdir(parents=True, exist_ok=True)
    atomic_write(cursor_path, json.dumps({"version": 1, "seen_event_ids": sorted(seen)},
                                         ensure_ascii=False, indent=2) + "\n")
    pending = [event.get("event_id") for event in events if event.get("event_id") not in acked]
    if pending:
        atomic_write(gate_path, json.dumps({"version": 1, "pending_event_ids": pending},
                                           ensure_ascii=False, indent=2) + "\n")
    else:
        gate_path.unlink(missing_ok=True)
    return emitted


def watch_agent_events(events_path, cursor_path, acknowledgements_path, gate_path, handoff_path, replay_pending=False):
    gate_path = Path(gate_path)
    gate_path.parent.mkdir(parents=True, exist_ok=True)
    gate_lock = Path(f"{gate_path}.lock")
    for _ in range(100):
        try:
            gate_lock.mkdir()
            break
        except FileExistsError:
            time.sleep(0.05)
    else:
        raise TimeoutError(f"dispatch gate lock remains held: {gate_lock}")
    try:
        return _watch_agent_events_locked(events_path, cursor_path,
                                          acknowledgements_path, gate_path,
                                          handoff_path, replay_pending=replay_pending)
    finally:
        gate_lock.rmdir()


def acknowledge_agent_event(events_path, acknowledgements_path, event_id, resolution):
    if resolution not in ("acknowledged", "revised"):
        raise ValueError("event resolution must be 'acknowledged' or 'revised'")
    event_ids = {item.get("event_id") for item in _read_json_lines(events_path)}
    if event_id not in event_ids:
        raise ValueError(f"unknown agent event: {event_id}")
    path = Path(acknowledgements_path)
    existing = _read_json_lines(path)
    if any(item.get("event_id") == event_id for item in existing):
        return False
    append_agent_event_row = {"event_id": event_id, "resolution": resolution,
                              "at": datetime.now(timezone.utc).isoformat()}
    path.parent.mkdir(parents=True, exist_ok=True)
    lock = Path(f"{path}.lock")
    for _ in range(100):
        try:
            lock.mkdir()
            break
        except FileExistsError:
            time.sleep(0.05)
    else:
        raise TimeoutError(f"agent acknowledgement lock remains held: {lock}")
    try:
        with path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(append_agent_event_row, ensure_ascii=False) + "\n")
    finally:
        lock.rmdir()
    return True


def record_agent_score_result(root, registration_id, task_id, result, evidence="",
                              events_path="", mid_batch=False, gate_path=""):
    path = registered_agent_score_path(root, registration_id)
    before = read_agent_score_state(root, registration_id)
    updated = record_score_result(path, task_id, result, evidence)
    if before["score"] != updated["score"] and events_path:
        append_agent_event(events_path, {
            "event_id": str(uuid.uuid4()), "registration_id": registration_id,
            "cause": "mid_batch_score_change" if mid_batch else "batch_score_change",
            "task_id": task_id, "previous_registration_id": registration_id,
            "current_registration_id": registration_id,
            "previous_score": before["score"], "current_score": updated["score"],
            "previous_level": score_task_level(before["score"]),
            "current_level": score_task_level(updated["score"]),
            "at": datetime.now(timezone.utc).isoformat(),
        }, gate_path)
    return {**updated, "registration_id": registration_id}


def _record_score_result(path, task_id, result, evidence=""):
    """Update shared Worker score from planner feedback or hard runtime evidence."""
    if result not in ("accept", "reject", "no_fault", "hard_failure", "success"):
        raise ValueError(f"unsupported score result: {result}")
    state = read_score_state(path)
    failures = state["failures"]
    if result == "hard_failure":
        count = int(failures.get(task_id, 0)) + 1
        failures[task_id] = count
        if count in (2, 3):
            state["score"] = max(1, state["score"] - 1)
    elif result == "success":
        failures.pop(task_id, None)
    elif result == "accept":
        failures.pop(task_id, None)
        state["success_window"].append(task_id)
        if len(state["success_window"]) >= 5:
            if all(state["success_window"][-5:]):
                state["score"] = min(5, state["score"] + 1)
            state["success_window"] = []
    elif result == "reject":
        state["success_window"] = []
    # no_fault is deliberately excluded from both scoring windows and penalties.
    state["feedback"].append({"task_id": task_id, "result": result,
                              "evidence": evidence, "at": datetime.now(timezone.utc).isoformat()})
    state["feedback"] = state["feedback"][-20:]
    atomic_write(path, json.dumps(state, ensure_ascii=False, indent=2) + "\n")
    return {**score_summary(state), "task_id": task_id, "result": result}


def record_score_result(path, task_id, result, evidence="", progress_path="", audit_ns=""):
    path = Path(path)
    lock = Path(f"{path}.lock")
    for _ in range(100):
        try:
            lock.mkdir()
            break
        except FileExistsError:
            time.sleep(0.05)
    else:
        raise TimeoutError(f"score state lock remains held: {lock}")
    try:
        if progress_path and audit_ns:
            if not update_planner_review(progress_path, task_id, audit_ns, result):
                return {**score_summary(read_score_state(path)), "task_id": task_id,
                        "result": result, "duplicate": True}
        return _record_score_result(path, task_id, result, evidence)
    finally:
        lock.rmdir()


def build_ghost_closeout_prompt(task_id, verify, progress_file, host_git=False):
    git_policy = ("Runtime owns Git writes: do not stage or commit."
                  if host_git else "After passing verification, commit only existing task changes; never edit their contents.")
    return f"""GHOST CLOSEOUT for original task {task_id} only. Verify/finalize existing work; do not implement or create another task.

- Inspect Git status/diff/log and run verification: {verify or 'the task verification command'}.
- Never modify, create, or delete source, tests, docs, config, or other implementation files. No fixes, workarounds, or other project-changing commands; if implementation is needed or unexpected changes exist, return blocked.
- {git_policy} Never commit unverified or failed work. Leave runtime-owned {progress_file} and tasks.done untouched and unstaged; their changes do not block closeout.
- Return exactly ONE JSON object to the adapter result file (not prose; CLI exit status alone is not proof):
  Verified changes: {{"outcome":"complete","changed":true,"commit_message":"type(scope): summary","verification_evidence":"actual command and result"}}
  Verified no changes: {{"outcome":"no_change","changed":false,"no_change_reason":"specific reason","verification_evidence":"actual command and result"}}
  Unfinished/failed: {{"outcome":"blocked","diagnostic":"concrete blocker: cause=<root cause>, verify_output=<failed error>, next_action=<planner suggestion>"}} (or scope_blocked/guidance when applicable).
- On failure/blocker, provide concise ASCII diagnosis for Planner: state exact failing command, error line, and recommended ticket breakdown or target expansion.
- A commit_message is required only for finished, verified changes. Runtime audits file changes and attributes this result to {task_id}.
"""


@serialize_workspace_state
def workspace_snapshot(work_dir, base):
    changed = subprocess.run(["git", "-C", work_dir, "diff", "--name-only", "-z", base],
                             capture_output=True, check=False).stdout
    untracked = subprocess.run(["git", "-C", work_dir, "ls-files", "--others", "--exclude-standard", "-z"],
                               capture_output=True, check=False).stdout
    paths = {os.fsdecode(item) for item in changed.split(b"\0") + untracked.split(b"\0") if item}
    paths = {name for name in paths
             if not name.startswith((".worker-ghosts/", ".worker-ghost-state",
                                     "nightshift/.worker-ghosts/", "nightshift/.worker-ghost-state"))}
    snapshot = {}
    for name in paths:
        path = Path(work_dir) / name
        if path.is_symlink():
            snapshot[name] = ("symlink", os.readlink(path), path.lstat().st_mode & 0o777)
        elif path.is_file():
            snapshot[name] = ("file", path.read_bytes(), path.stat().st_mode & 0o777)
        else:
            snapshot[name] = None
    return snapshot


def pop_task(queue_path):
    """Remove one task under the same directory lock used by reorder_queue.sh."""
    queue = Path(queue_path)
    lock = Path(f"{queue}.lock")
    for attempt in range(50):
        try:
            lock.mkdir()
            break
        except FileExistsError:
            time.sleep(0.1)
    else:
        raise TimeoutError(f"queue lock remains held: {lock}")

    try:
        lines = queue.read_text(encoding="utf-8").splitlines(keepends=True) if queue.exists() else []
        for index, line in enumerate(lines):
            if line.strip() and not line.lstrip().startswith("#"):
                task = line.rstrip("\r\n")
                atomic_write(queue, "".join(lines[:index] + lines[index + 1:]))
                return task
        return ""
    finally:
        lock.rmdir()


def restore_task_front(queue_path, task):
    """Put a claimed task back at the queue head without racing queue editors."""
    queue = Path(queue_path)
    lock = Path(f"{queue}.lock")
    for attempt in range(50):
        try:
            lock.mkdir()
            break
        except FileExistsError:
            time.sleep(0.1)
    else:
        raise TimeoutError(f"queue lock remains held: {lock}")
    try:
        lines = queue.read_text(encoding="utf-8").splitlines(keepends=True) if queue.exists() else []
        insertion = next((index for index, line in enumerate(lines)
                          if line.strip() and not line.lstrip().startswith("#")), len(lines))
        task_line = task.rstrip("\r\n") + "\n"
        atomic_write(queue, "".join(lines[:insertion] + [task_line] + lines[insertion:]))
    finally:
        lock.rmdir()


def insert_task_front(queue_path, task):
    """Insert a normal task before pending work under the queue claim lock."""
    queue = Path(queue_path)
    task_line = task.strip("\r\n")
    task_id = task_line.split("|", 1)[0].strip()
    if not task_id or "\n" in task_line or task_id.startswith("#"):
        raise ValueError("queue insertion requires one task line with a task ID")

    lock = Path(f"{queue}.lock")
    for _ in range(50):
        try:
            lock.mkdir()
            break
        except FileExistsError:
            time.sleep(0.1)
    else:
        raise TimeoutError(f"queue lock remains held: {lock}")

    try:
        lines = queue.read_text(encoding="utf-8").splitlines(keepends=True) if queue.exists() else []
        queued_ids = {line.split("|", 1)[0].strip() for line in lines
                      if line.strip() and not line.lstrip().startswith("#")}
        if task_id in queued_ids:
            return False
        insertion = next((index for index, line in enumerate(lines)
                          if line.strip() and not line.lstrip().startswith("#")), len(lines))
        atomic_write(queue, "".join(lines[:insertion] + [task_line + "\n"] + lines[insertion:]))
        return True
    finally:
        lock.rmdir()


def parse_agent_registry(raw, defaults=None):
    if not raw.strip():
        return []
    profiles = json.loads(raw)
    if not isinstance(profiles, list) or not profiles:
        raise ValueError("agent registry must be a non-empty JSON array")
    agents, seen = [], set()
    for item in profiles:
        if not isinstance(item, dict):
            raise ValueError("each agent registry entry must be an object")
        unknown = set(item) - {"id", "adapter", "command", "model", "effort", "args", "env", "overrides", "default"}
        if unknown:
            raise ValueError(f"agent profile has unknown fields: {', '.join(sorted(unknown))}")
        for key in ("id", "adapter", "command", "model", "effort"):
            if key in item and not isinstance(item[key], str):
                raise ValueError(f"agent {key} must be a string")
        agent_id = item.get("id", "").strip()
        adapter = item.get("adapter", "").strip()
        command = item.get("command", "").strip()
        if not agent_id or agent_id in seen:
            raise ValueError("agent registry names must be non-empty and unique")
        if not adapter:
            raise ValueError(f"agent {agent_id} requires an adapter name")
        if not command:
            raise ValueError(f"agent {agent_id} requires a command")
        extra_args = item.get("args", [])
        environment = item.get("env", {})
        overrides = item.get("overrides", {})
        if not isinstance(overrides, dict):
            raise ValueError(f"agent {agent_id} overrides must be an object")
        overrides = {**overrides, **{key: item[key] for key in ("model", "effort") if key in item}}
        if not isinstance(extra_args, list) or not all(isinstance(arg, str) for arg in extra_args):
            raise ValueError(f"agent {agent_id} args must be an array of strings")
        if not isinstance(environment, dict) or not all(isinstance(k, str) and isinstance(v, str)
                                                      and k and "=" not in k and "\0" not in k + v
                                                      for k, v in environment.items()):
            raise ValueError(f"agent {agent_id} env must map valid names to strings")
        seen.add(agent_id)
        agents.append({
            "id": agent_id, "adapter": adapter, "command": command,
            "args": extra_args, "env": environment,
            "overrides": overrides,
            "model": str(overrides.get("model", "")),
        })
    return agents


def load_agent_registry(config_file):
    """Load the shared agent-name map and order default registrations first."""
    path = Path(config_file)
    if not path.is_file():
        return []
    profiles = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(profiles, dict):
        raise ValueError(f"{path} must contain an object keyed by agent name")
    normalized = []
    for name, profile in profiles.items():
        if not isinstance(name, str) or not name.strip() or not isinstance(profile, dict):
            raise ValueError("agent registry entries must map non-empty names to objects")
        unknown = set(profile) - {"default", "worker", "agent"}
        if unknown:
            raise ValueError(f"agent {name} has unknown fields: {', '.join(sorted(unknown))}")
        if "default" in profile and not isinstance(profile["default"], bool):
            raise ValueError(f"agent {name} default must be a boolean")
        worker = profile.get("worker", {})
        agent = profile.get("agent")
        if not isinstance(worker, dict):
            raise ValueError(f"agent {name} worker config must be an object")
        worker_unknown = set(worker) - {"task_timeout_seconds"}
        if worker_unknown:
            raise ValueError(f"agent {name} worker config has unknown fields: {', '.join(sorted(worker_unknown))}")
        timeout = worker.get("task_timeout_seconds")
        if timeout is not None and (isinstance(timeout, bool) or not isinstance(timeout, (int, float))
                                    or not math.isfinite(timeout) or timeout <= 0):
            raise ValueError(f"agent {name} worker.task_timeout_seconds must be a positive number")
        if not isinstance(agent, dict):
            raise ValueError(f"agent {name} requires an agent config object")
        agent_unknown = set(agent) - {"adapter", "command", "args", "env", "config"}
        if agent_unknown:
            raise ValueError(f"agent {name} agent config has unknown fields: {', '.join(sorted(agent_unknown))}")
        adapter = agent.get("adapter", "")
        command = agent.get("command", "")
        if not isinstance(adapter, str) or not adapter.strip():
            raise ValueError(f"agent {name} requires an adapter name")
        if not isinstance(command, str) or not command.strip():
            raise ValueError(f"agent {name} requires an agent command")
        extra_args = agent.get("args", [])
        environment = agent.get("env", {})
        config = agent.get("config", {})
        if not isinstance(extra_args, list) or not all(isinstance(arg, str) for arg in extra_args):
            raise ValueError(f"agent {name} agent.args must be an array of strings")
        if not isinstance(environment, dict) or not all(
                isinstance(key, str) and isinstance(value, str) and key and "=" not in key
                and "\0" not in key + value for key, value in environment.items()):
            raise ValueError(f"agent {name} agent.env must map valid names to strings")
        if not isinstance(config, dict):
            raise ValueError(f"agent {name} agent.config must be an object")
        for key in ("model", "effort"):
            if key in config and not isinstance(config[key], str):
                raise ValueError(f"agent {name} agent.config.{key} must be a string")
        normalized.append({
            "id": name,
            "default": bool(profile.get("default", False)),
            "adapter": adapter.strip(),
            "command": command.strip(),
            "args": extra_args,
            "env": environment,
            "worker": worker,
            "overrides": {**config, "command": command.strip(), "args": extra_args},
            "model": config.get("model", ""),
        })
    normalized.sort(key=lambda profile: not profile["default"])
    return normalized


ADAPTER_PROTOCOL_VERSION = 1
ADAPTER_TERMINAL_STATUSES = {"complete", "closeout_required", "no_response",
                             "step_budget_exceeded", "execution_timeout",
                             "callback_timeout", "cancellation_failed",
                             "failed", "cancelled"}


def adapter_callback(command, callback, request, extra_args=(), timeout=30):
    """Invoke one versioned JSON callback; stdout is protocol data, stderr is diagnostics."""
    process = subprocess.Popen(
        [command, *extra_args, callback], stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=(os.name == "posix"),
    )
    try:
        stdout, stderr = process.communicate(json.dumps(request), timeout=timeout)
    except subprocess.TimeoutExpired:
        if os.name == "posix":
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except subprocess.TimeoutExpired:
                pass
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=0.25)
            except subprocess.TimeoutExpired:
                pass
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        else:
            process.kill()
        try:
            process.communicate(timeout=1)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
        raise
    completed = subprocess.CompletedProcess(process.args, process.returncode,
                                            stdout=stdout, stderr=stderr)
    if completed.returncode:
        raise RuntimeError(f"adapter {callback} failed ({completed.returncode}): {completed.stderr.strip()}")
    try:
        response = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise ValueError(f"adapter {callback} returned invalid JSON: {error}") from error
    if not isinstance(response, dict):
        raise ValueError(f"adapter {callback} response must be an object")
    if "error" in response:
        detail = response["error"]
        raise RuntimeError(f"adapter {callback} error: {detail}")
    return response


def execute_adapter_protocol(command, request, extra_args=(), trace=None,
                             callback_timeout=30, execution_timeout=1800,
                             start_callback="start"):
    """Run one external adapter execution through config, asynchronous start and status."""
    base = {"protocol_version": ADAPTER_PROTOCOL_VERSION, **request}
    if trace:
        base["trace"] = str(trace)
    try:
        configuration = adapter_callback(command, "get_config", {
            "protocol_version": ADAPTER_PROTOCOL_VERSION,
            "registration_id": base.get("registration_id", "default"),
            "overrides": base.get("overrides", {}),
            **({"trace": str(trace)} if trace else {}),
        }, extra_args, timeout=callback_timeout)
    except subprocess.TimeoutExpired:
        return {"status": "callback_timeout", "stopped": True,
                "diagnostic": "get_config callback process exceeded its response deadline"}
    version = configuration.get("protocol_version")
    if version != ADAPTER_PROTOCOL_VERSION:
        raise ValueError(f"adapter protocol version {version!r} is incompatible; expected {ADAPTER_PROTOCOL_VERSION}")
    if not isinstance(configuration.get("initial_score"), int) or not 1 <= configuration["initial_score"] <= 5:
        raise ValueError("adapter get_config response requires initial_score from 1 through 5")
    if not isinstance(configuration.get("limits", {}), dict):
        raise ValueError("adapter get_config limits must be an object")
    if not isinstance(configuration.get("configuration", {}), dict):
        raise ValueError("adapter get_config configuration must be an object")
    callback_timeout = configuration.get("limits", {}).get(
        "callback_timeout_seconds", callback_timeout)
    if not isinstance(callback_timeout, (int, float)) or callback_timeout <= 0:
        raise ValueError("adapter callback_timeout_seconds must be positive")
    base["configuration"] = configuration.get("configuration", {})
    base["limits"] = {**configuration.get("limits", {}), **base.get("limits", {})}
    try:
        execution = adapter_callback(command, start_callback, base, extra_args,
                                     timeout=callback_timeout)
    except subprocess.TimeoutExpired:
        return {"status": "cancellation_failed", "stopped": False,
                "diagnostic": "start callback timed out before returning an execution handle"}
    execution_id, status_location = execution.get("execution_id"), execution.get("status_location")
    if not isinstance(execution_id, str) or not execution_id or not isinstance(status_location, str) or not status_location:
        raise ValueError("adapter start response requires execution_id and status_location")
    started = time.monotonic()

    def cancel(reason):
        try:
            response = adapter_callback(command, "cancel", {
                "protocol_version": ADAPTER_PROTOCOL_VERSION,
                "execution_id": execution_id, "reason": reason,
                "stop_timeout_seconds": callback_timeout,
                **({"trace": str(trace)} if trace else {}),
            }, extra_args, timeout=callback_timeout)
            stopped = response.get("stopped") is True
            return {"status": reason if stopped else "cancellation_failed",
                    "execution_id": execution_id, "stopped": stopped,
                    **({} if stopped else {"diagnostic": "adapter did not confirm termination"})}
        except (OSError, RuntimeError, subprocess.TimeoutExpired, ValueError) as error:
            return {"status": "cancellation_failed", "execution_id": execution_id,
                    "stopped": False, "diagnostic": str(error)}

    log_file = None
    log_offset = 0
    status_dir = Path(status_location).parent
    last_heartbeat = started
    last_log_activity = started

    def stream_adapter_output():
        nonlocal log_file, log_offset, last_log_activity
        if log_file is None:
            candidates = sorted(status_dir.glob("*.log"))
            if candidates:
                log_file = candidates[0]
        if log_file and log_file.is_file():
            try:
                with log_file.open("r", encoding="utf-8", errors="replace") as stream:
                    stream.seek(log_offset)
                    chunk = stream.read()
                    if chunk:
                        log_offset = stream.tell()
                        last_log_activity = time.monotonic()
                        sys.stderr.write(chunk)
                        sys.stderr.flush()
            except OSError:
                pass

    while True:
        elapsed = time.monotonic() - started
        if elapsed >= execution_timeout:
            return cancel("execution_timeout")
        try:
            status = adapter_callback(command, "status", {
                "protocol_version": ADAPTER_PROTOCOL_VERSION,
                "execution_id": execution_id, "status_location": status_location,
                **({"trace": str(trace)} if trace else {}),
            }, extra_args, timeout=callback_timeout)
        except subprocess.TimeoutExpired:
            return cancel("callback_timeout")
        stream_adapter_output()
        now = time.monotonic()
        if now - last_log_activity >= 15 and now - last_heartbeat >= 15:
            agent_label = base.get("registration_id") or base.get("adapter", "adapter")
            print(f"[WORKER] {agent_label} is still executing ({int(now - started)}s elapsed)...",
                  file=sys.stderr, flush=True)
            last_heartbeat = now
        current = status.get("status")
        if current in ADAPTER_TERMINAL_STATUSES:
            stream_adapter_output()
            if current == "complete" and not status.get("result_location"):
                raise ValueError("completed adapter status requires result_location")
            return {**status, "execution_id": execution_id,
                    "adapter_initial_score": configuration["initial_score"],
                    "elapsed_seconds": time.monotonic() - started}
        if current != "running":
            raise ValueError(f"adapter returned unsupported status: {current!r}")
        interval = configuration.get("limits", {}).get("poll_interval_seconds", 1)
        try:
            interval = max(0.01, float(interval))
        except (TypeError, ValueError) as error:
            raise ValueError("adapter poll_interval_seconds must be a positive number") from error
        time.sleep(min(interval, max(0, execution_timeout - (time.monotonic() - started))))


def should_failover_agent(exit_code, result_text):
    """Only no_response and confirmed timeout statuses may select another registration."""
    if isinstance(result_text, dict):
        payload = result_text
    else:
        try:
            payload = json.loads(result_text)
        except (TypeError, json.JSONDecodeError):
            payload = {}
    status = payload.get("status", "") if isinstance(payload, dict) else ""
    if status in ("cancellation_failed", "step_budget_exceeded") or exit_code == 125:
        return False
    return status in ("no_response", "execution_timeout", "callback_timeout")


def read_agent_state(path):
    path = Path(path)
    if not path.exists():
        return {"version": 1, "active_agent_id": "", "consecutive_no_response": 0, "task_exclusions": {}}
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {"version": 1, "active_agent_id": "", "consecutive_no_response": 0, "task_exclusions": {}}
    if not isinstance(state, dict):
        state = {}
    state.setdefault("version", 1)
    state.setdefault("active_agent_id", "")
    state.setdefault("consecutive_no_response", 0)
    state.setdefault("task_exclusions", {})
    return state


def task_header(title):
    """Return task header before first pipe."""
    return str(title or "").split("|", 1)[0]


def task_header_has_tag(title, tag_pattern):
    """Check if task header before first pipe matches tag_pattern."""
    return bool(re.search(tag_pattern, task_header(title), re.I))


def is_research_task(title):
    """Check if task header contains [RESEARCH]."""
    return task_header_has_tag(title, r"\[RESEARCH\]")


def agent_supports_task(agent, title, constraints=""):
    """Apply an optional registration task-scope filter before agent routing."""
    if not agent.get("overrides", {}).get("research_only", False):
        return True
    return scratch_review.is_review(title) or is_research_task(title)


def execute_registered_agents(args, agents, state_path, rollback):
    state = read_agent_state(state_path)
    task_exclusions = state.get("task_exclusions", {}).get(args.task_id, [])
    task_agents = [agent for agent in agents
                   if agent_supports_task(agent, getattr(args, "title", ""),
                                          getattr(args, "constraints", ""))]
    available = [agent for agent in task_agents
                 if agent["id"] not in task_exclusions]
    if not available:
        args.awaiting_agent = True
        print("[WORKER] AGENT_UNAVAILABLE: no registered agent supports this task.")
        return 0, False

    active_id = state.get("active_agent_id", "")
    consecutive_no_response = int(state.get("consecutive_no_response", 0))

    start_index = 0
    if active_id:
        for idx, a in enumerate(available):
            if a["id"] == active_id:
                start_index = idx
                break
    else:
        for idx, a in enumerate(available):
            if a.get("default", False):
                start_index = idx
                break

    current_index = start_index
    while True:
        agent = available[current_index]
        attempt = argparse.Namespace(**vars(args))
        attempt.attempt_id = str(uuid.uuid4())
        snapshot_root = Path(getattr(args, "attempt_snapshot_root", "") or
                             (_scratch_state_root(args.work_dir) / "attempts" / args.task_id))
        attempt.attempt_baseline = str(snapshot_root / attempt.attempt_id / "baseline")
        attempt.adapter_artifact_dir = str(Path(attempt.attempt_baseline).parent / "adapter")
        args.attempt_id = attempt.attempt_id
        args.attempt_baseline = attempt.attempt_baseline
        args.adapter_artifact_dir = attempt.adapter_artifact_dir
        try:
            capture_attempt_baseline(attempt.work_dir, args.task_id, attempt.attempt_id,
                                     attempt.attempt_baseline)
        except Exception:
            args.recovery_failed = True
            raise
        args.recovery_failed = False
        attempt.adapter = agent["adapter"]
        attempt.executable = agent["command"]
        attempt.adapter_command = sys.executable
        attempt.adapter_args = [str(Path(__file__).resolve().parent / "adapters" / f"{agent['adapter']}.py")]
        attempt.timeout = agent.get("worker", {}).get("task_timeout_seconds", args.timeout)
        attempt.model = agent.get("model", "")
        attempt.extra_args = agent.get("args", [])
        attempt.agent_env = agent.get("env", {})
        attempt.agent_id = agent["id"]
        args.executed_agent_id = agent["id"]
        attempt.executed_agent_id = agent["id"]
        attempt.agent_overrides = agent.get("overrides", {})
        for output_name in ("output_file", "result_file"):
            output = Path(getattr(attempt, output_name, "") or ".")
            if output.is_file():
                output.unlink()
        print(f"[WORKER] Launching registered agent {agent['id']} ({agent['adapter']}, model={agent.get('model') or 'default'})...")
        code = execute_adapter_task(attempt)
        output_path = Path(attempt.output_file)
        result_path = Path(attempt.result_file) if attempt.result_file else output_path
        output_text = output_path.read_text(encoding="utf-8", errors="replace") if output_path.exists() else ""
        result_text = result_path.read_text(encoding="utf-8", errors="replace") if result_path.exists() else ""
        try:
            parsed_result = json.loads(result_text)
        except (TypeError, json.JSONDecodeError):
            parsed_result = {}
        try:
            parsed_output = json.loads(output_text)
        except (TypeError, json.JSONDecodeError):
            parsed_output = {}

        status_val = parsed_output.get("status", "") if isinstance(parsed_output, dict) else ""
        outcome_val = parsed_result.get("outcome", "") if isinstance(parsed_result, dict) else ""
        error_code_val = parsed_result.get("error_code", "") if isinstance(parsed_result, dict) else ""
        actions_used = parsed_result.get("actions_used", 0) if isinstance(parsed_result, dict) else 0

        is_no_response = (
            status_val == "no_response"
            or outcome_val == "no_response"
            or error_code_val == "no_response"
            or (code != 0 and actions_used == 0
                and outcome_val in ("", "failure", "invalid_result")
                and status_val not in ("execution_timeout", "callback_timeout",
                                       "cancellation_failed", "step_budget_exceeded"))
        )

        if not is_no_response and not should_failover_agent(code, parsed_output):
            state["consecutive_no_response"] = 0
            state["active_agent_id"] = agent["id"]
            if state.get("task_exclusions", {}).pop(args.task_id, None) is not None:
                pass
            atomic_write(state_path, json.dumps(state, ensure_ascii=False, indent=2) + "\n")
            return code, False

        rollback(attempt.attempt_baseline, attempt.attempt_id)
        if is_no_response:
            consecutive_no_response += 1
            state["consecutive_no_response"] = consecutive_no_response
            current_index = (current_index + 1) % len(available)
            next_agent = available[current_index]
        else:
            task_exclusions.append(agent["id"])
            state.setdefault("task_exclusions", {})[args.task_id] = task_exclusions
            available = [candidate for candidate in task_agents
                         if candidate["id"] not in task_exclusions]
            if not available:
                args.awaiting_agent = True
                atomic_write(state_path, json.dumps(state, ensure_ascii=False, indent=2) + "\n")
                return 0, False
            current_index = 0
            next_agent = available[current_index]
        state["active_agent_id"] = next_agent["id"]
        atomic_write(state_path, json.dumps(state, ensure_ascii=False, indent=2) + "\n")

        max_no_response = len(available)
        if is_no_response and consecutive_no_response >= max_no_response:
            print(f"[WORKER] AGENT_NO_RESPONSE: {consecutive_no_response} consecutive switches without agent response; reporting to planner.", file=sys.stderr)
            args.agent_no_response = True
            return 0, True

        reason = "no response" if is_no_response else "timed out"
        print(f"[WORKER] Agent {agent['id']} {reason}; switching to {next_agent['id']} (attempt {consecutive_no_response}/{max_no_response})...", file=sys.stderr)


def progress_bar(current, total, width=30):
    if total <= 0:
        return "[------------------------------] 0% (0/0)"
    filled = current * width // total
    return f"[{'#' * filled}{'-' * (width - filled)}] {current * 100 // total}% ({current}/{total})"


def poll_next(args):
    """Claim one queue item and return the scheduler's external state as fields."""
    fields = [""] * 16
    if Path(args.pause).exists() or (getattr(args, "dispatch_gate", "")
                                     and Path(args.dispatch_gate).exists()):
        fields[0] = "paused"
        return fields
    raw = pop_task(args.tasks)
    def count(path):
        source = Path(path)
        if not source.exists():
            return 0
        return sum(bool(line.strip()) and not line.lstrip().startswith("#")
                   for line in source.read_text(encoding="utf-8").splitlines())
    done = count(args.done)
    remaining = count(args.tasks) + bool(raw)
    total = done + remaining
    fields[:8] = ["task" if raw else "idle", raw, str(done), str(remaining),
                  str(total), str(done + 1 if raw else done),
                  str(done * 100 // total if total else 0), progress_bar(done, total)]
    if raw:
        fields[8] = raw.split("|", 1)[0].replace(" ", "") or "TASK-UNKNOWN"
        level = re.search(r"LEVEL:\s*([0-9])", raw)
        fields[9] = level.group(1) if level else "1"
        for index, key in ((10, "TARGET"), (11, "VERIFY"),
                           (12, "FORBIDDEN_SHORTCUTS"), (13, "CONSTRAINTS")):
            fields[index] = task_field(raw, key)
        targets = [item.strip() for item in fields[10].split(",") if item.strip()]
        file_count = 0
        for target in ([] if scratch_review.is_review(raw) else targets):
            path = Path(args.work_dir) / target
            file_count += max(1, sum(1 for item in path.rglob("*") if item.is_file())) if path.is_dir() else 1
        file_count = max(1, file_count)
        fields[14] = str(file_count)
        fields[15] = str(args.step_base + file_count * args.step_per_file)
    return fields


def task_field(raw, key):
    if key not in FIELD_NAMES:
        raise ValueError(f"unknown task field: {key}")
    # A pipe only separates fields when followed by a recognized field name.
    start = re.search(rf"(?:^|\|\s*){key}:\s*", raw)
    if start is None:
        return ""
    rest = raw[start.end():]
    end = re.search(r"\s*\|\s*(?:" + "|".join(FIELD_NAMES) + r"):\s*", rest)
    return (rest[:end.start()] if end else rest).strip()


@serialize_workspace_state
def write_status(args):
    state = {
        "worker_id": args.worker_id,
        "os": args.os_name,
        "platform": args.platform,
        "work_dir": args.work_dir,
        "state": args.state,
        "current_task": args.current_task or None,
        "elapsed_seconds": args.elapsed,
        "progress": {
            "current_index": args.current_index,
            "total_tasks": args.total_tasks,
            "remaining": args.remaining,
            "percentage": args.percentage,
            "bar": args.bar,
        },
        "pid": args.pid,
        "model": args.model,
        "cli": args.cli,
        "timeout_seconds": args.timeout,
        "worker_score": getattr(args, "worker_score", 5),
        "recommended_task_level": score_task_level(getattr(args, "worker_score", 5)),
        "last_heartbeat": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    atomic_write(args.path, json.dumps(state, ensure_ascii=False, indent=2) + "\n")


def display_status(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    pid = data.get("pid")
    alive = False
    if isinstance(pid, int) and pid > 0:
        try:
            os.kill(pid, 0)
            alive = True
        except OSError:
            pass
    flag = f"[ALIVE (PID: {pid})]" if alive else "[DEAD / INACTIVE]"
    print(f"Worker State   : {data.get('state', 'UNKNOWN')} {flag}")
    print(f"Current Task   : {data.get('current_task') or 'None'} "
          f"({data.get('elapsed_seconds', 0)}s elapsed)")
    print(f"Progress       : {data.get('progress', {}).get('bar', '[---]')}")
    print(f"Worker Score   : {data.get('worker_score', 5)}/5 | Recommended task level: L{data.get('recommended_task_level', 1)}")


def run_with_timeout(seconds, command):
    """Run a command with inherited output and terminate its process group on timeout."""
    if not command:
        raise ValueError("run-timeout requires a command after --")
    options = {}
    if os.name == "nt":
        options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        options["start_new_session"] = True
    process = subprocess.Popen(command, **options)
    try:
        return_code = process.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        try:
            if os.name == "nt":
                process.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                os.killpg(process.pid, signal.SIGTERM)
        except (OSError, ValueError):
            process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            if os.name == "nt":
                process.kill()
            else:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except OSError:
                    process.kill()
            process.wait()
        return 124
    return 128 + abs(return_code) if return_code < 0 else return_code


@contextmanager
def temporary_git_index(work_dir, tree="HEAD"):
    """Use Git's tree machinery without staging or replacing the user's index."""
    with tempfile.TemporaryDirectory(prefix="nightshift-index-") as directory:
        environment = {**os.environ, "GIT_INDEX_FILE": str(Path(directory) / "index")}
        def git(*argv):
            return subprocess.run(["git", "-C", str(work_dir), *argv], env=environment,
                                  check=True, capture_output=True).stdout
        git("read-tree", tree)
        yield git


@serialize_workspace_state
def worktree_tree(work_dir):
    with temporary_git_index(work_dir) as git:
        git("add", "-A", "--", ".")
        return git("write-tree").decode().strip()


def attempt_audit_trees(args):
    snapshot = getattr(args, "baseline_snapshot", "") or getattr(args, "attempt_baseline", "")
    if not snapshot:
        return None
    metadata = json.loads((Path(snapshot) / "snapshot.json").read_text(encoding="utf-8"))
    if Path(metadata.get("work_dir", "")).resolve() != Path(args.work_dir).resolve():
        raise ValueError("audit baseline belongs to another workspace")
    if not metadata.get("audit_tree"):
        raise ValueError("attempt baseline is missing its audit tree")
    return metadata["audit_tree"], worktree_tree(args.work_dir)


def changed_tree_paths(work_dir, before, after):
    return _git_names(work_dir, "diff", "--name-only", "-z", before, after)


def run_code_index(work_dir, arguments, *, capture_output=False):
    """Run the bundled indexer through the parser environment installed with NightShift."""
    tools = Path(__file__).resolve().parent / "tools"
    python = tools / ".venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    if not python.is_file():
        raise FileNotFoundError(
            f"code-index environment is missing: {python}; "
            f"repair it with bash {tools / 'setup.sh'} or rerun the NightShift installer")
    return subprocess.run([str(python), str(tools / "index.py"), "--repo", str(work_dir),
                           *arguments], check=False, text=True,
                          capture_output=capture_output)


def code_index_inputs(work_dir, paths):
    """Whether changed paths can affect the configured code index."""
    tools = Path(__file__).resolve().parent / "tools"
    registry = json.loads((tools / "languages.json").read_text(encoding="utf-8"))
    extensions = {extension.lower() for spec in registry.values()
                  for extension in spec.get("extensions", [])}
    special = {"cmakelists.txt", "makefile", "gnumakefile", "package.swift",
               "package.json", "tsconfig.json", "pyproject.toml", "cargo.toml",
               "go.mod", "requirements.txt", "code-index.toml"}
    settings_suffixes = {".xcconfig", ".xcscheme"}
    for name in paths:
        path = Path(name)
        if (path.suffix.lower() in extensions or path.name.lower() in special
                or path.suffix.lower() in settings_suffixes
                or name in {"nightshift/tools/index.py", "nightshift/tools/languages.json",
                            "nightshift/tools/requirements.txt"}):
            return True
    return False


def ensure_code_index_after_commit(work_dir, previous_head):
    """Refresh the index after a script-owned code commit; indexing failure is non-fatal."""
    try:
        current_head = subprocess.run(["git", "-C", str(work_dir), "rev-parse", "HEAD"],
                                      check=True, capture_output=True, text=True).stdout.strip()
        if current_head == previous_head:
            return
        changed = _git_names(work_dir, "diff", "--name-only", "-z", previous_head, current_head)
        if not code_index_inputs(work_dir, changed):
            return
        result = run_code_index(work_dir, ["ensure"])
        if result.returncode:
            detail = result.stderr.strip() if result.stderr else ""
            print("[WORKER] WARNING: task commit succeeded, but code-index refresh failed; "
                  "a later query or explicit ensure can rebuild it. " + detail,
                  file=sys.stderr)
    except (OSError, ValueError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        print("[WORKER] WARNING: task commit succeeded, but code-index refresh failed; "
              f"a later query or explicit ensure can rebuild it. {error}", file=sys.stderr)


def finalize_task_git(args):
    trees = attempt_audit_trees(args)
    if trees:
        before, after = trees
        changed = changed_tree_paths(args.work_dir, before, after)
        targets = [item.strip().rstrip("/") for item in args.target.split(",") if item.strip()]
        changed = [name for name in changed if any(name == target or name.startswith(target + "/")
                                                  for target in targets)]
        original = set(changed_tree_paths(args.work_dir, args.base, before))
        snapshot = getattr(args, "baseline_snapshot", "") or getattr(args, "attempt_baseline", "")
        metadata = json.loads((Path(snapshot) / "snapshot.json").read_text(encoding="utf-8"))
        original.update(metadata.get("index_changes", []))
        overlap = original.intersection(changed)
        if overlap:
            raise ValueError("Task overlaps pre-existing user changes; refusing to commit mixed work: "
                             + ", ".join(sorted(overlap)))
        if not changed:
            return 0
        previous_head = subprocess.run(["git", "-C", args.work_dir, "rev-parse", "HEAD"],
                                       check=True, capture_output=True, text=True).stdout.strip()
        with temporary_git_index(args.work_dir) as git:
            git("add", "-A", "--", *changed)
            if git("diff", "--cached", "--name-only", "-z"):
                git("commit", "-m", args.commit_message, "-m",
                    f"Task: {args.task_id}\nScope: {args.target}\nVerification: {args.verify}")
        # Refresh only the task entries; unrelated staged/unstaged user work remains intact.
        subprocess.run(["git", "-C", args.work_dir, "reset", "-q", "HEAD", "--", *changed],
                       check=True, capture_output=True)
        ensure_code_index_after_commit(args.work_dir, previous_head)
        return 0
    targets = [item.strip() for item in args.target.split(",") if item.strip()]
    for target in targets:
        subprocess.run(["git", "-C", args.work_dir, "add", "-A", "--", target],
                       check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    staged = subprocess.run(["git", "-C", args.work_dir, "diff", "--cached", "--quiet"],
                            check=False)
    if staged.returncode:
        previous_head = subprocess.run(["git", "-C", args.work_dir, "rev-parse", "HEAD"],
                                       check=True, capture_output=True, text=True).stdout.strip()
        title = args.title
        commit_type = ("fix" if task_header_has_tag(title, r"\[CODE:\s*FIX\]") else
                       "refactor" if task_header_has_tag(title, r"\[CODE:\s*REFACTOR\]") else
                       "docs" if is_research_task(title) else
                       "test" if task_header_has_tag(title, r"\[DIAGNOSTIC\]") else "feat")
        commit = subprocess.run([
            "git", "-C", args.work_dir, "commit", "-m",
            getattr(args, "commit_message", "") or f"{commit_type}(worker): complete {args.task_id}", "-m",
            f"Task: {args.task_id} (Level {args.level})\nScope: {args.target or '<modified files>'}\n"
            f"Verification: {args.verify or '<worker post-flight>'}\n"
            "Attempts: wrapper-managed | Confidence: pending audit",
        ], check=False)
        if commit.returncode:
            print(f"[WORKER] Failed to commit task changes for {args.task_id}.", file=sys.stderr)
            return 1
        ensure_code_index_after_commit(args.work_dir, previous_head)
    return 0


def bounded_verify_evidence(output):
    """Bound persisted combined-output evidence; subprocess capture is unchanged."""
    limit = 16384
    encoded = output.encode("utf-8")
    return {"verify_output": encoded[-limit:].decode("utf-8", errors="ignore"),
            "verify_output_truncated": len(encoded) > limit,
            "verify_output_limit_bytes": limit,
            "stream_mode": "stdout_stderr_combined"}


def finalize_structured_adapter_result(args, result):
    outcome = result.get("outcome")
    if outcome == "no_change":
        reason = result.get("no_change_reason")
        if not isinstance(reason, str) or not reason.strip():
            raise ValueError("no_change result requires a non-empty no_change_reason")
        trees = attempt_audit_trees(args)
        if trees:
            tracked = changed_tree_paths(args.work_dir, *trees)
            untracked = []
        else:
            tracked = subprocess.run(["git", "-C", args.work_dir, "diff", "--name-only", args.base],
                                     capture_output=True, text=True, check=False).stdout.splitlines()
            untracked = subprocess.run(["git", "-C", args.work_dir, "ls-files", "--others",
                                        "--exclude-standard"], capture_output=True, text=True,
                                       check=False).stdout.splitlines()
        runtime_file = re.compile(r"(?:^|/)(?:nightshift/)?(?:progress\.log|tasks\.txt|tasks\.done|worker\..*\.status|\.worker.*|\.preview(?:/|$)|\.reports(?:/|$))")
        changes = [name for name in tracked + untracked if not runtime_file.search(name)]
        if changes:
            raise ValueError("no_change result conflicts with workspace changes")
        return 0
    if outcome in ("scope_blocked", "guidance", "blocked", "failure", "closeout_required"):
        return 126 if outcome == "closeout_required" else 1
    if outcome != "complete":
        raise ValueError(f"unsupported adapter result outcome: {outcome!r}")
    message = result.get("commit_message")
    if not isinstance(message, str) or not message.strip():
        return 126
    if args.verify and not re.match(r"^(REPORT_ONLY|none)$", args.verify, re.I):
        verify_code, verify_output = run_verify(args.verify, args.work_dir,
                                                args.worker_port, args.timeout)
        if verify_output:
            print(verify_output, end="" if verify_output.endswith("\n") else "\n")
        if verify_code:
            reason = f"Verification failed ({verify_code}): {args.verify}; refusing script-owned commit."
            result["runtime_error"] = {"phase": "verification", "message": reason,
                                       "code": verify_code, **bounded_verify_evidence(verify_output)}
            print(f"[WORKER] {reason}", file=sys.stderr)
            return 1
    audit_args = argparse.Namespace(
        work_dir=args.work_dir, base=args.base, target=args.target,
        constraints=getattr(args, "constraints", ""), title=args.title,
        level=int(args.level or 1), allow_dirty=True,
        baseline_snapshot=getattr(args, "attempt_baseline", ""),
    )
    audit_code, audit_reason = git_audit(audit_args)
    if audit_code:
        result["runtime_error"] = {"phase": "pre_commit_audit", "message": audit_reason,
                                   "code": audit_code}
        print(f"[WORKER] Pre-commit audit failed: {audit_reason}", file=sys.stderr)
        return audit_code
    args.commit_message = message
    try:
        return finalize_task_git(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", b"") or str(error)
        if isinstance(detail, bytes):
            detail = detail.decode("utf-8", "replace")
        result["runtime_error"] = {"phase": "git_finalization", "message": detail,
                                   "code": getattr(error, "returncode", 1)}
        print(f"[WORKER] Git finalization failed: {detail}", file=sys.stderr)
        return 1


def claim_ghost_allowance(state_path, task_id, registration_id):
    path = Path(state_path)
    lock = Path(f"{path}.lock")
    for _ in range(100):
        try:
            lock.mkdir()
            break
        except FileExistsError:
            time.sleep(0.05)
    else:
        raise TimeoutError(f"ghost state lock remains held: {lock}")
    try:
        state = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {
            "version": 1, "issued": {}}
        if state.get("version") != 1 or not isinstance(state.get("issued"), dict):
            raise ValueError(f"invalid ghost state file: {path}")
        if task_id in state["issued"]:
            return False
        state["issued"][task_id] = {
            "registration_id": registration_id,
            "issued_at": datetime.now(timezone.utc).isoformat(),
        }
        atomic_write(path, json.dumps(state, ensure_ascii=False, indent=2) + "\n")
        return True
    finally:
        lock.rmdir()


def run_adapter_ghost(args, main_request, main_status, main_result):
    task_id = args.task_id
    registration_id = main_request["registration_id"]
    state_path = getattr(args, "ghost_state_path", "") or str(
        _runtime_state_root(args.work_dir) / ".worker-ghost-state.json")
    if not claim_ghost_allowance(state_path, task_id, registration_id):
        print(f"[WORKER] Ghost allowance already used for {task_id}.", file=sys.stderr)
        return 126, {}, None
    ghost_budget = int(getattr(args, "ghost_step_budget", 0) or
                       os.environ.get("WORKER_GHOST_STEP_BUDGET", "25"))
    remaining_deadline = max(float(os.environ.get("WORKER_GHOST_TIMEOUT", "180")),
                             args.timeout - float(main_status.get("elapsed_seconds", 0)))
    if ghost_budget <= 0 or remaining_deadline <= 0:
        return 126, {}, None
    output_root = Path(getattr(args, "ghost_artifact_root", "") or
                       (_runtime_state_root(args.work_dir) / ".worker-ghosts")) / re.sub(
                           r"[^A-Za-z0-9_.-]", "_", task_id)
    output_root.mkdir(parents=True, exist_ok=True)
    ghost_dir = output_root / f"ghost-{uuid.uuid4().hex}"
    ghost_dir.mkdir(parents=True, exist_ok=False)
    prompt_path = ghost_dir / "closeout-prompt.txt"
    atomic_write(prompt_path, build_ghost_closeout_prompt(
        task_id, args.verify, args.progress_file, host_git=True))
    ghost_result = ghost_dir / "result.json"
    before = workspace_snapshot(args.work_dir, args.base)
    request = {
        **main_request,
        "kind": "closeout",
        "execution_id": main_status["execution_id"],
        "prompt_path": str(prompt_path.resolve()),
        "output_dir": str(ghost_dir.resolve()),
        "result_path": str(ghost_result.resolve()),
        "limits": {**main_request["limits"],
                   "action_limit": ghost_budget,
                   "execution_timeout_seconds": remaining_deadline,
                   "closeout_deadline_seconds": remaining_deadline},
    }
    status = execute_adapter_protocol(
        getattr(args, "adapter_command", args.executable), request,
        getattr(args, "adapter_args", getattr(args, "extra_args", ())),
        callback_timeout=getattr(args, "callback_timeout", 30),
        execution_timeout=remaining_deadline, start_callback="closeout",
    )
    after = workspace_snapshot(args.work_dir, args.base)
    if before != after:
        changed = sorted(name for name in before.keys() | after.keys()
                         if before.get(name) != after.get(name))
        details = "; ".join(f"{name}: {before.get(name)!r} -> {after.get(name)!r}"
                             for name in changed)
        print("[WORKER] Ghost closeout changed implementation files; rejecting result: "
              + details,
              file=sys.stderr)
        return 126, {}, ghost_dir
    if status.get("status") not in ("complete", "failed") or not status.get("result_location"):
        return 126, {}, ghost_dir
    source = Path(status["result_location"])
    if not source.is_file():
        raise ValueError(f"ghost result file does not exist: {source}")
    result = json.loads(source.read_text(encoding="utf-8"))
    if not isinstance(result, dict):
        return 126, {}, ghost_dir
    if not ghost_result.exists():
        shutil.copy2(source, ghost_result)
    if status.get("status") == "failed" and result.get("outcome") not in ("blocked", "scope_blocked", "guidance", "failure"):
        return 126, result, ghost_dir
    code = finalize_structured_adapter_result(args, result)
    return code, result, ghost_dir


def execute_adapter_task(args):
    """Adapt a registered task to the external protocol and persist its structured result."""
    output = Path(args.output_file)
    output.parent.mkdir(parents=True, exist_ok=True)
    result_path = Path(args.result_file or output.with_name("adapter-result.json"))
    result_path.parent.mkdir(parents=True, exist_ok=True)
    attempt_id = getattr(args, "attempt_id", "") or str(uuid.uuid4())
    result_path.unlink(missing_ok=True)
    artifact_root = getattr(args, "adapter_artifact_dir", "")
    if not artifact_root:
        artifact_root = output.parent
        work_dir = Path(args.work_dir).resolve()
        if Path(artifact_root).resolve().is_relative_to(work_dir):
            artifact_root = work_dir.parent / ".nightshift-adapter-artifacts" / args.task_id
    adapter_output_dir = Path(artifact_root) / attempt_id
    adapter_output_dir.mkdir(parents=True, exist_ok=True)
    if not getattr(args, "attempt_baseline", ""):
        inside_git = subprocess.run(["git", "-C", args.work_dir, "rev-parse", "--is-inside-work-tree"],
                                    capture_output=True, check=False).returncode == 0
        if inside_git:
            args.attempt_baseline = str(adapter_output_dir / "baseline")
            capture_attempt_baseline(args.work_dir, args.task_id, attempt_id, args.attempt_baseline)
    prompt_path = Path(args.prompt_file)
    adapter_command = getattr(args, "adapter_command", args.executable)
    adapter_args = getattr(args, "adapter_args", getattr(args, "extra_args", ()))
    overrides = getattr(args, "agent_overrides", None)
    if overrides is None:
        try:
            overrides = json.loads(getattr(args, "overrides_json", "{}"))
        except json.JSONDecodeError as error:
            raise ValueError(f"adapter overrides must be valid JSON: {error}") from error
        if not isinstance(overrides, dict):
            raise ValueError("adapter overrides must be a JSON object")
    args.adapter_command = adapter_command
    args.adapter_args = adapter_args
    request = {
        "registration_id": getattr(args, "agent_id", "") or args.adapter,
        "overrides": overrides,
        "task_id": args.task_id,
        "title": getattr(args, "title", ""),
        "target": getattr(args, "target", ""),
        "verify": getattr(args, "verify", ""),
        "task_version": getattr(args, "task_version", hashlib.sha256(
            (getattr(args, "title", "") or args.task_id).encode("utf-8")).hexdigest()[:16]),
        "attempt_id": attempt_id,
        "working_directory": str(Path(args.work_dir).resolve()),
        "prompt_path": str(prompt_path.resolve()),
        "output_dir": str(adapter_output_dir.resolve()),
        "result_path": str(result_path.resolve()),
        "environment": {**getattr(args, "agent_env", {}),
                        "PORT": args.worker_port, "TEST_PORT": args.worker_port},
        "limits": {"execution_timeout_seconds": args.timeout,
                   "action_limit": args.step_budget},
    }
    status = execute_adapter_protocol(adapter_command, request, adapter_args,
                                      callback_timeout=getattr(args, "callback_timeout", 30),
                                      execution_timeout=getattr(args, "execution_timeout", args.timeout))
    score_root = getattr(args, "agent_score_state_file", "")
    if score_root and status.get("adapter_initial_score") is not None:
        initialize_agent_score(score_root, request["registration_id"],
                               status["adapter_initial_score"])
    if status.get("result_location"):
        result_path = Path(status["result_location"])
        if not result_path.is_file():
            raise ValueError(f"adapter result file does not exist: {result_path}")
        destination = Path(args.result_file or output.with_name("adapter-result.json"))
        if result_path.resolve() != destination.resolve():
            shutil.copy2(result_path, destination)
    result_path = Path(args.result_file or output.with_name("adapter-result.json"))
    result = json.loads(result_path.read_text(encoding="utf-8")) if result_path.exists() else {}
    terminal_failure = (isinstance(result, dict) and result.get("outcome") in
                        ("blocked", "scope_blocked", "guidance", "failure"))
    if terminal_failure and status.get("status") in ("complete", "closeout_required", "failed"):
        code = finalize_structured_adapter_result(args, result)
    elif status.get("status") in ("complete", "closeout_required"):
        if not isinstance(result, dict):
            raise ValueError("adapter result must be a JSON object")
        code = (finalize_structured_adapter_result(args, result)
                if status.get("status") == "complete" else 126)
        if code == 126 and (result.get("outcome") == "closeout_required"
                            or (result.get("outcome") == "complete"
                                and not str(result.get("commit_message", "")).strip())):
            code, rescued_result, ghost_dir = run_adapter_ghost(args, request, status, result)
            if rescued_result:
                result = rescued_result
                atomic_write(result_path, json.dumps(result, ensure_ascii=False) + "\n")
    else:
        code = {"no_response": 124, "step_budget_exceeded": 125,
                "closeout_required": 126, "execution_timeout": 124,
                "callback_timeout": 124, "cancellation_failed": 124}.get(
                    status["status"], 1)
    if isinstance(result, dict):
        atomic_write(result_path, json.dumps(result, ensure_ascii=False) + "\n")
    wrapper = {"status": status["status"], "stopped": status.get("stopped"), "result": result}
    if getattr(args, "adapter_artifact_dir", ""):
        try:
            terminal_dir = adapter_output_dir / ".runtime-terminal"
            terminal_dir.mkdir(mode=0o700, exist_ok=True)
            terminal_dir.chmod(0o700)
            durable_result = terminal_dir / "result.json"
            manifest_path = terminal_dir / "manifest.json"
            atomic_write(durable_result, json.dumps(result, ensure_ascii=False) + "\n")
            durable_result.chmod(0o600)
            manifest = {"schema_version": 1, "task_id": args.task_id, "attempt_id": attempt_id,
                        "adapter": args.adapter, "registration_id": request["registration_id"],
                        "adapter_terminal_status": status["status"], "runtime_exit_code": code,
                        "terminal_result": durable_result.name}
            atomic_write(manifest_path, json.dumps(manifest, ensure_ascii=False) + "\n")
            manifest_path.chmod(0o600)
            wrapper["terminal_artifacts"] = {"result": str(durable_result.resolve()),
                                             "manifest": str(manifest_path.resolve())}
        except OSError as error:
            wrapper["terminal_artifact_error"] = {"phase": "terminal_persistence", "message": str(error)}
            print(f"[WORKER] WARNING: terminal evidence persistence failed: {error}", file=sys.stderr)
    output.write_text(json.dumps(wrapper, ensure_ascii=False), encoding="utf-8")
    return code


def build_task_prompt(task, task_id, level, target, verify, forbidden, constraints,
                      max_steps, target_file_count, step_base, step_per_file, progress_file):
    if scratch_review.is_review(task):
        return scratch_review.prompt(task, task_id)
    contract = Path(__file__).resolve().parent.parent / ".skills/universal-build-verify.md"
    contract_text = contract.read_text(encoding="utf-8") if contract.is_file() else ""
    authority = (f"LAUNCHER CONTRACT ({contract}):\n{contract_text}"
                 if contract_text else f"Read launcher contract: {contract}")
    return f"""Execute one NightShift ticket unattended. The ticket and launcher contract govern.

TASK_ID: {task_id} | LEVEL: {level or '1'}
{task}

START
- Read nightshift/.worker.notes.md. Applicable skills under .agents/skills/: [CODE] engineering/tdd/SKILL.md (+ engineering/codebase-design/SKILL.md for interfaces); [DIAGNOSTIC] engineering/diagnosing-bugs/SKILL.md; [RESEARCH] engineering/research/SKILL.md + nightshift/explore/SKILL.md.
- Report-first: runtime filters active `.reports/<slug>/SUMMARY.md` against source revisions. Read only relevant active reports and `03-micro.md` before exploring code; reuse verified `path#Lline` and investigate only gaps. Never use `.archive/` or `.invalid/`. If you prove a report stale, you MAY move its directory to `.reports/.invalid/<slug>`; only the runtime deletes it after audited success or restores it after rollback if still valid. Do not delete/archive reports yourself or move unrelated files. Reports never override current code or the ticket.

RULES
- Budget: {max_steps} actions ({step_base}+{target_file_count}×{step_per_file}); reserve 3 for verify/closeout. Per-invocation timeout. Emit `[EXEC] <action>` when required; no fabricated markers, bundled actions, or budget evasion.
- Writes only in TARGET ({target or 'declared paths'}), except moving a proven-stale report into `.reports/.invalid/`. Honor FORBIDDEN_SHORTCUTS/CONSTRAINTS; wider code scope requires [SCOPE_BLOCKED] with `path#Lline`.
- Deletion caps: L3 40 lines, L2 150, L1 TARGET only. [CODE: FIX] must not change tests; [CODE: REFACTOR] must not reduce assertions. [DIAGNOSTIC] may add only a compile-clean repro test; [RESEARCH] reads source and writes reports only.
- Never weaken tests, hide errors, fake behavior, mask races with sleeps, use sudo/host package managers, or destructively reset. Do not edit {progress_file}; runtime owns `progress.log`/`tasks.done` (leave unstaged).

CLOSEOUT
- Run {verify or 'task verification'} and state actual results. [DEAD_END] requires isolated passing proof. At most 3 implementation attempts; on blocker, return [BLOCKED] with cause.
- Never stage/commit or change Worker score. Runtime verifies, audits and commits using your message; Planner accepts. Do not adjust scope for the adapter.
- Return exactly one JSON object with no text before or after it. Fields: `outcome`=complete|closeout_required|blocked|scope_blocked|guidance|failure|no_change; `changed`=boolean; `verification_evidence`; `commit_message`; `no_change_reason` when relevant; `attempts`=integer; `confidence`=HIGH|MED|LOW. `complete`=finished/verified; `no_change`=no edits needed; `closeout_required`=verification/message pending. If verification fails, return blocked or failure; closeout_required cannot repair failed tests. Never invent evidence/messages from logs.

{authority}
"""


def env_bool(name, default="0"):
    return os.environ.get(name, default).lower() in ("1", "true", "yes", "on")


def acquire_worker_lock(path, pid):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    while True:
        try:
            descriptor = os.open(str(path), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        except FileExistsError:
            try:
                old_pid = int(path.read_text(encoding="utf-8").strip())
                if old_pid > 0:
                    os.kill(old_pid, 0)
                    print(f"ERROR: Worker daemon already running with PID {old_pid}.", file=sys.stderr)
                    return False
            except ProcessLookupError:
                pass
            except PermissionError:
                print("ERROR: Existing worker PID cannot be checked; assuming it is active.", file=sys.stderr)
                return False
            except (ValueError, FileNotFoundError):
                pass
            try:
                path.unlink()
            except FileNotFoundError:
                pass
            continue
        else:
            try:
                os.write(descriptor, f"{pid}\n".encode("ascii"))
            finally:
                os.close(descriptor)
            return True


def worker_daemon(root):
    root = Path(root).resolve()
    config = os.environ
    worker_id = config.get("WORKER_ID", "default")

    def resolve(name, default):
        path = Path(config.get(name) or default)
        return path if path.is_absolute() else root / path

    tasks_file = resolve("TASKS_FILE", "tasks.txt")
    done_file = resolve("TASKS_DONE_FILE", "tasks.done")
    progress_file = resolve("PROGRESS_FILE", "progress.log")
    pause_file = resolve("PAUSE_FILE", "PAUSE")
    dispatch_gate_file = resolve("AGENT_DISPATCH_GATE_FILE", ".worker-dispatch-gate.json")
    status_file = resolve("STATUS_FILE", f"worker.{worker_id}.status")
    log_file = resolve("LOG_FILE", f".worker.{worker_id}.log")
    pid_file = resolve("PID_FILE", f".worker.{worker_id}.pid")
    if (root / "nightshift/adapters/agents.json").is_file():
        agent_config_default = "nightshift/adapters/agents.json"
    elif (root / "scripts/adapters/agents.json").is_file():
        agent_config_default = "scripts/adapters/agents.json"
    else:
        agent_config_default = "agents/agents.json"
    agent_config_file = resolve("AGENT_CONFIG_FILE", agent_config_default)
    agents = load_agent_registry(agent_config_file)
    if not agents and tasks_file.is_file() and any(
            line.strip() and not line.lstrip().startswith("#")
            for line in tasks_file.read_text(encoding="utf-8").splitlines()):
        raise ValueError(f"{agent_config_file} must register at least one executable adapter")
    safe_worker_id = re.sub(r"[^A-Za-z0-9_.-]", "_", worker_id)
    agent_state_path = resolve(
        "AI_AGENT_STATE_FILE", f".worker-agent-state.{safe_worker_id}.json")
    ghost_state_path = resolve(
        "WORKER_GHOST_STATE_FILE", f".worker-ghost-state.{safe_worker_id}.json")
    ghost_artifact_root = resolve("WORKER_GHOST_ARTIFACT_DIR", ".worker-ghosts")
    score_state_path = resolve("WORKER_SCORE_FILE", ".worker-score.json")
    agent_score_state_path = resolve("WORKER_AGENT_SCORES_DIR", ".worker-agent-scores")
    agent_events_path = resolve("WORKER_AGENT_EVENTS_FILE", ".worker-agent-events.jsonl")
    timeout_value = float(config.get("TASK_TIMEOUT", "1800"))
    timeout = int(timeout_value) if timeout_value.is_integer() else timeout_value
    step_base = int(config.get("STEP_BASE", "12"))
    step_per_file = int(config.get("STEP_PER_FILE", "5"))
    poll_interval = float(config.get("POLL_INTERVAL", "5"))
    worker_port = config.get("WORKER_PORT", "3001")
    work_dir = root
    system = platform.system()
    linux_release = Path("/etc/os-release")
    is_ubuntu = (linux_release.is_file()
                 and "ubuntu" in linux_release.read_text(encoding="utf-8", errors="replace").lower())
    detected_os = "mac" if system == "Darwin" else "ubuntu" if system == "Linux" and is_ubuntu else "linux" if system == "Linux" else "unknown"
    active_os = config.get("OS_TYPE", "auto")
    if not active_os or active_os == "auto":
        active_os = detected_os
    platform_name = ("macOS (Darwin)" if active_os in ("mac", "darwin") else
                     "Linux (Ubuntu)" if active_os == "ubuntu" else f"Linux ({active_os})")
    step_args = {"step_base": step_base, "step_per_file": step_per_file}

    for name, default in (("CI", "1"), ("PAGER", "cat"), ("NO_COLOR", "1"),
                          ("DEBIAN_FRONTEND", "noninteractive"), ("TERM", "dumb"),
                          ("NIGHTSHIFT_WORKER", "1")):
        os.environ[name] = config.get(name, default)

    pid = os.getpid()
    if not acquire_worker_lock(pid_file, pid):
        return 1
    stop_requested = threading.Event()
    final_state = "STOPPED"
    def request_stop(signum, frame):
        stop_requested.set()
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    if hasattr(signal, "SIGHUP"):
        try:
            signal.signal(signal.SIGHUP, request_stop)
        except (ValueError, OSError):
            pass

    status_lock = _workspace_state_lock
    latest_status = None
    running_since = None
    heartbeat_stop = threading.Event()

    def update_status(state, task="", elapsed=0, current=0, total=0, remaining=0,
                      percentage="0%", bar=""):
        nonlocal latest_status, running_since
        with status_lock:
            if state == "RUNNING":
                if latest_status is None or latest_status.state != "RUNNING" or latest_status.current_task != task:
                    running_since = time.monotonic() - elapsed
            else:
                running_since = None
            status_args = argparse.Namespace(
                path=str(status_file), worker_id=worker_id, os_name=active_os,
                platform=platform_name, work_dir=str(work_dir),
                state=state, current_task=task, elapsed=int(elapsed),
                current_index=int(current), total_tasks=int(total), remaining=int(remaining),
                percentage=percentage, bar=bar or progress_bar(int(current), int(total)),
                pid=pid, model="", cli="registered-adapters", timeout=timeout,
                worker_score=read_score_state(score_state_path)["score"],
            )
            write_status(status_args)
            latest_status = status_args

    def refresh_running_status():
        while not heartbeat_stop.wait(1):
            with status_lock:
                if latest_status is not None and latest_status.state == "RUNNING" and running_since is not None:
                    latest_status.elapsed = int(time.monotonic() - running_since)
                    try:
                        write_status(latest_status)
                    except OSError as error:
                        print(f"[WORKER] Heartbeat write failed: {error}", file=sys.stderr)

    heartbeat = threading.Thread(target=refresh_running_status, name="worker-heartbeat", daemon=True)

    def append_line(path, line):
        with Path(path).open("a", encoding="utf-8") as stream:
            stream.write(line + "\n")

    def stamp():
        return datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    try:
        heartbeat.start()
        for path in (tasks_file, done_file, progress_file):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch(exist_ok=True)
        (root / ".reports/.archive").mkdir(parents=True, exist_ok=True)
        inside = subprocess.run(["git", "-C", str(work_dir), "rev-parse", "--is-inside-work-tree"],
                                capture_output=True, check=False).returncode == 0
        if inside:
            subprocess.run(["git", "-C", str(work_dir), "tag", "-f", "nightshift-session-base", "HEAD"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(["git", "-C", str(root), "tag", "-f", "nightshift-session-base", "HEAD"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        print("=" * 78)
        print(" 24-Hour Unattended AI Worker Daemon Started")
        print(f" Worker ID: {worker_id} | OS: {platform_name} (CPUs: {os.cpu_count() or 4})")
        print(f" PID: {pid} | Registered adapters: {len(agents)}")
        print(f" WorkDir: {work_dir}")
        print(f" Tasks: {tasks_file} | Status: {status_file}")
        print(f" Progress: {progress_file} | Log: {log_file}")
        print("=" * 78)
        update_status("IDLE", bar="[------------------------------] 0%")
        was_busy = False
        idle_status_shown = False
        run_once = env_bool("RUN_ONCE", "0")
        pending_planner = None
        waiting_for_response = False
        recovery_halted_task = ""
        scratch_checked_at = float("-inf")

        while not stop_requested.is_set():
            if recovery_halted_task and not pause_file.exists():
                state_title = f"{recovery_halted_task} (recovery halted); awaiting revised queue or intervention"
                update_status("RECOVERY_FAILED", state_title)
                stop_requested.wait(poll_interval)
                head_task = ""
                if tasks_file.is_file():
                    lines = [l.strip() for l in tasks_file.read_text(encoding="utf-8").splitlines()
                             if l.strip() and not l.lstrip().startswith("#")]
                    head_task = lines[0].split("|", 1)[0].replace(" ", "") if lines else ""
                if head_task != recovery_halted_task:
                    recovery_halted_task = ""
                else:
                    continue
            if waiting_for_response and not pause_file.exists():
                state = read_agent_state(agent_state_path)
                if state.get("consecutive_no_response", 0) >= len(agents):
                    update_status("AGENT_NO_RESPONSE", "Waiting for agent response")
                    stop_requested.wait(poll_interval)
                    continue
                # Back off before retrying the requeued task to avoid a hot loop.
                stop_requested.wait(poll_interval)
                waiting_for_response = False
            if (agents and not pending_planner and not pause_file.exists()
                    and not dispatch_gate_file.exists() and time.monotonic() - scratch_checked_at >= 60):
                try:
                    scratch_review.check(tasks_file.parent, tasks_file, insert_task_front, atomic_write)
                except (OSError, ValueError, TimeoutError) as error:
                    print(f"[SCRATCH_REVIEW] monitor deferred: {error}", file=sys.stderr)
                scratch_checked_at = time.monotonic()
            poll_args = argparse.Namespace(tasks=str(tasks_file), done=str(done_file),
                                           pause=str(pause_file), work_dir=str(work_dir),
                                           dispatch_gate=str(dispatch_gate_file),
                                           **step_args)
            fields = poll_next(poll_args)
            if fields[0] == "paused":
                if dispatch_gate_file.exists():
                    print(f"[{stamp()}] [PLANNER_REVIEW_REQUIRED] Dispatch held by {dispatch_gate_file}.")
                    update_status("PAUSED_FOR_PLANNER", f"Review agent event in {dispatch_gate_file}", percentage="0%", bar="[PLANNER]")
                    stop_requested.wait(poll_interval)
                    continue
                print(f"[{stamp()}] [PAUSED] Found {pause_file}. Sleeping...")
                update_status("PAUSED", f"Paused via {pause_file} file", percentage="0%", bar="[PAUSED]")
                stop_requested.wait(poll_interval)
                continue
            raw_task, done_count, remaining = fields[1], int(fields[2]), int(fields[3])
            total, current = int(fields[4]), int(fields[5])
            percent, bar = f"{fields[6]}%", fields[7]
            if fields[0] == "idle":
                if pending_planner:
                    failed_task, reason = pending_planner
                    state = {"SCOPE": "SCOPE_BLOCKED", "GUIDANCE": "AWAITING_GUIDANCE"}.get(reason, "BLOCKED")
                    update_status(state, f"{failed_task} ({reason}); awaiting Planner")
                    if not idle_status_shown:
                        print(f"[AWAITING_PLANNER] {failed_task}: daemon remains alive; waiting for revised queue work.")
                        idle_status_shown = True
                    if run_once:
                        break
                    stop_requested.wait(poll_interval)
                    continue
                update_status("IDLE", current=done_count, total=total, percentage="100%", bar=bar)
                if was_busy:
                    print(f"\n{'=' * 78}\n[{stamp()}] [QUEUE_DRAINED] {bar}")
                    print(f"[IDLE] Queue is empty (Total completed: {done_count} tasks).\n{'=' * 78}")
                    was_busy, idle_status_shown = False, True
                    if run_once:
                        print("[WORKER] Exiting because RUN_ONCE is enabled.")
                        break
                elif not idle_status_shown:
                    print(f"[{stamp()}] [IDLE] Queue is empty (Total completed: {done_count} tasks). Waiting for tasks in {tasks_file}...")
                    idle_status_shown = True
                    if run_once:
                        print("[WORKER] Queue is empty and RUN_ONCE is enabled. Exiting.")
                        break
                stop_requested.wait(poll_interval)
                continue

            was_busy, idle_status_shown = True, False
            pending_planner = None
            scratch_checked_at = float("-inf")
            task, task_id, level = raw_task, fields[8], fields[9]
            target, verify, forbidden, constraints = fields[10], fields[11], fields[12], fields[13]
            target_file_count, max_steps = int(fields[14]), int(fields[15])
            head = subprocess.run(["git", "-C", str(work_dir), "rev-parse", "HEAD"],
                                  capture_output=True, text=True, check=False)
            base = head.stdout.strip() if head.returncode == 0 else "HEAD~1"
            started = time.time()
            is_research = is_research_task(task)
            is_diagnostic = task_header_has_tag(task, r"\[DIAGNOSTIC\]")
            # Script owns report freshness and retirement, including before research.
            refresh_active_reports(root, work_dir)
            if root.resolve() != work_dir.resolve():
                refresh_active_reports(work_dir, work_dir)
            print(f"\n{'=' * 78}\n[{stamp()}] >>> [Task {current} of {total} (In Progress) | Completed: {done_count}/{total} ({fields[6]}%)]")
            print(f">>> Progress: {bar}\n>>> Task: {task}")
            print(f">>> Step Budget: {max_steps} steps (Base: {step_base} + {target_file_count} files * {step_per_file})\n{'=' * 78}")
            update_status("RUNNING", task, current=current, total=total,
                          remaining=max(0, remaining - 1), percentage=percent, bar=bar)
            prompt = build_task_prompt(task, task_id, level, target, verify, forbidden,
                                       constraints, max_steps, target_file_count,
                                       step_base, step_per_file, progress_file)
            with tempfile.TemporaryDirectory(prefix="nightshift-task-") as temporary:
                prompt_path = Path(temporary) / "prompt.txt"
                output_path = Path(temporary) / "cli-output.log"
                result_path = Path(temporary) / "last-message.txt"
                prompt_path.write_text(prompt, encoding="utf-8")
                print(f"[WORKER] Launching registered adapters (StepBudget: {max_steps}, WorkDir: {work_dir})...")
                cli_args = argparse.Namespace(
                    adapter="", executable="", work_dir=str(work_dir),
                    prompt_file=str(prompt_path), output_file=str(output_path),
                    result_file=str(result_path),
                    timeout=timeout, model="", effort="",
                    step_budget=max_steps, worker_port=worker_port, target=target,
                    task_id=task_id, title=task, level=level, verify=verify,
                    base=base, progress_file=str(progress_file),
                    ghost_state_path=str(ghost_state_path),
                    ghost_artifact_root=str(ghost_artifact_root),
                    agent_score_state_file=str(agent_score_state_path),
                    agent_events_file=str(agent_events_path),
                    attempt_snapshot_root=str(_scratch_state_root(root) / "attempts" /
                                              re.sub(r"[^A-Za-z0-9_.-]", "_", task_id)),
                    recovery_failed=False,
                    recovery_halted=False,
                )

                def record_task_score(result, evidence=""):
                    registration_id = getattr(cli_args, "executed_agent_id", "")
                    if (registration_id and registered_agent_score_path(
                            agent_score_state_path, registration_id).is_file()):
                        return record_agent_score_result(
                            agent_score_state_path, registration_id, task_id,
                            result, evidence, agent_events_path,
                            gate_path=dispatch_gate_file,
                        )
                    return record_score_result(score_state_path, task_id, result, evidence)

                def recover_current_attempt(snapshot=None, attempt_id=None):
                    nonlocal final_state
                    try:
                        adapter_dir = Path(getattr(cli_args, "adapter_artifact_dir", ""))
                        adapter_artifacts = (sorted(path for path in adapter_dir.rglob("*") if path.is_file())
                                             if adapter_dir.is_dir() else [])
                        recovered = rollback_failed_task(
                            root, work_dir, task_id,
                            (tasks_file, done_file, progress_file, log_file, status_file, agent_state_path,
                             ghost_state_path, ghost_artifact_root, score_state_path,
                             *adapter_artifacts),
                            snapshot if snapshot is not None else cli_args.attempt_baseline,
                            attempt_id or cli_args.attempt_id,
                        )
                        # A failed attempt cannot permanently retire a report.
                        settle_invalid_reports(root, work_dir, accepted=False)
                        if root.resolve() != work_dir.resolve():
                            settle_invalid_reports(work_dir, work_dir, accepted=False)
                        return recovered
                    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
                        cli_args.recovery_failed = True
                        print(f"[WORKER] Attempt recovery failed; stopping before another execution: {error}",
                              file=sys.stderr)
                        restore_task_front(tasks_file, raw_task)
                        append_line(progress_file,
                                    f"[RECOVERY_HALTED] {task_id}: baseline could not be restored; task requeued.")
                        final_state = "RECOVERY_FAILED"
                        update_status(final_state, f"{task_id} (baseline restore failed)",
                                      int(time.time() - started), current, total, remaining,
                                      percent, bar)
                        cli_args.recovery_halted = True
                        return None

                def recover_before_switch(snapshot, attempt_id):
                    if recover_current_attempt(snapshot, attempt_id) is None:
                        raise RuntimeError("attempt recovery failed; replacement execution was not started")
                cli_args.awaiting_planner = False
                cli_args.awaiting_agent = False
                cli_args.agent_no_response = False
                cli_args.agent_events_file = str(agent_events_path)
                cli_args.dispatch_gate_file = str(dispatch_gate_file)
                execution_error = ""
                try:
                    exit_code, has_event = execute_registered_agents(
                        cli_args, agents, agent_state_path, recover_before_switch,
                    )
                except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
                    execution_error = str(error)
                    print(f"[WORKER] CLI execution failed: {error}", file=sys.stderr)
                    exit_code = 2
                result_output = result_path if result_path.is_file() and result_path.stat().st_size else output_path
                duration = int(time.time() - started)
                append_line(log_file, f"=== [{stamp()}] Task: {task} (Duration: {duration}s, Exit: {exit_code}) ===")
                with Path(log_file).open("a", encoding="utf-8") as stream:
                    stream.write(output_path.read_text(encoding="utf-8", errors="replace") if output_path.exists() else "")
                    if execution_error:
                        stream.write(f"\n[WORKER] Runtime execution error: {execution_error}")
                    stream.write("\n")
                result_text = result_output.read_text(encoding="utf-8", errors="replace") if result_output.exists() else ""

                structured_outcome = ""
                structured_execution_status = ""
                failure_reason = execution_error
                cli_exit_code = None
                result_error_code = ""
                try:
                    payload = json.loads(result_text)
                    if isinstance(payload, dict):
                        structured_outcome = payload.get("outcome", "")
                        structured_execution_status = payload.get("status", "")
                        raw_cli_exit_code = payload.get("cli_exit_code")
                        if isinstance(raw_cli_exit_code, int) and not isinstance(raw_cli_exit_code, bool):
                            cli_exit_code = raw_cli_exit_code
                        result_error_code = str(payload.get("error_code", ""))
                        runtime_error = payload.get("runtime_error", {})
                        if isinstance(runtime_error, dict) and runtime_error.get("message"):
                            failure_reason = str(runtime_error["message"])
                except json.JSONDecodeError:
                    pass
                structured_status = {
                    "complete": "success", "no_change": "success",
                    "closeout_required": "closeout_required", "scope_blocked": "scope",
                    "guidance": "guidance", "blocked": "blocked", "failure": "error",
                    "cancellation_failed": "cancel_failed",
                }.get(structured_outcome, "")
                if structured_execution_status == "cancellation_failed":
                    structured_status = "cancel_failed"

                outcome = ("agent_no_response" if getattr(cli_args, "agent_no_response", False) else
                           "planner_gate" if cli_args.awaiting_planner else
                           "agent_unavailable" if cli_args.awaiting_agent else
                           "cancel_failed" if structured_status == "cancel_failed" else
                           "timeout" if exit_code == 124 else
                           "step_budget" if exit_code == 125 else
                           "closeout_required" if exit_code == 126 and structured_outcome == "closeout_required" else
                           "closeout_failed" if exit_code == 126 else
                           "scope" if structured_status == "scope" else
                           "blocked" if structured_status == "blocked" else
                           "guidance" if structured_status == "guidance" else
                           "cancel_failed" if structured_status == "cancel_failed" else
                           "error" if exit_code else
                           structured_status if structured_status else
                           "scope" if "[SCOPE_BLOCKED]" in result_text else
                           "blocked" if "[BLOCKED]" in result_text else
                           "guidance" if re.search(r"\[(?:NEED_GUIDANCE|NEED_DECOMPOSITION)\]", result_text) else
                           "success")
                stamp_now = stamp()
                if (scratch_review.is_review(task) and outcome not in
                        ("agent_no_response", "planner_gate", "agent_unavailable", "cancel_failed")):
                    error = "" if outcome == "success" else f"execution: {outcome}"
                    if not error:
                        try:
                            guard, reason = git_audit(argparse.Namespace(
                                work_dir=str(work_dir), base=base, target=target,
                                constraints=constraints, title=task, level=1,
                                baseline_snapshot=getattr(cli_args, "attempt_baseline", "")))
                            if guard:
                                raise ValueError(reason)
                            scratch_review.validate_report(tasks_file.parent, task_id)
                        except (OSError, ValueError, TypeError) as failure:
                            error = str(failure)
                    if error and getattr(cli_args, "attempt_baseline", ""):
                        if recover_current_attempt() is None:
                            if run_once:
                                break
                            recovery_halted_task = task_id
                            continue
                    scratch_review.finish(tasks_file.parent, task_id, atomic_write, error,
                        str(Path(cli_args.attempt_baseline).parent)
                        if not error and getattr(cli_args, "attempt_baseline", "") else "")
                    print(f"[SCRATCH_REVIEW] {'failed' if error else 'reviewed'}: fixed report overwritten")
                    update_status("IDLE")
                    continue
                if outcome not in ("success", "agent_no_response", "planner_gate", "agent_unavailable"):
                    pending_planner = (task_id, outcome.upper())
                if outcome == "agent_no_response":
                    restore_task_front(tasks_file, raw_task)
                    warning = ("=" * 80 + f"\n[AGENT_NO_RESPONSE] {task_id}: agent no response\n"
                               f"Time: {stamp_now} | Status: AGENT_NO_RESPONSE\n"
                               f"Task returned to {tasks_file}; all registered agents ({len(agents)}) returned no response.\n" + "=" * 80)
                    prepend_progress(progress_file, warning)
                    print(f"[{stamp_now}] [AGENT_NO_RESPONSE] agent no response; all registered agents ({len(agents)}) returned no response.")
                    update_status("AGENT_NO_RESPONSE", task, duration, current, total,
                                  remaining, percent, bar)
                    if run_once:
                        final_state = "AGENT_NO_RESPONSE"
                        break
                    waiting_for_response = True
                    stop_requested.wait(poll_interval)
                    continue
                elif outcome == "planner_gate":
                    restore_task_front(tasks_file, raw_task)
                    append_line(progress_file,
                                f"[PLANNER_REVIEW_REQUIRED] {task_id}: agent failover recorded; replacement waits for Planner acknowledgement.")
                    print(f"[{stamp_now}] [PLANNER_REVIEW_REQUIRED] Replacement agent is held until Planner acknowledges the change.")
                    update_status("PAUSED_FOR_PLANNER", task, duration, current, total,
                                  remaining, percent, bar)
                    if run_once:
                        final_state = "PAUSED_FOR_PLANNER"
                        break
                    stop_requested.wait(poll_interval)
                    continue
                elif outcome == "agent_unavailable":
                    restore_task_front(tasks_file, raw_task)
                    append_line(progress_file,
                                f"[AGENT_UNAVAILABLE] {task_id}: no eligible registration remains for this task; task returned to {tasks_file}. After fixing the adapter issue, remove this task from task_exclusions in {agent_state_path} to retry.")
                    update_status("WAITING_FOR_AGENT", task, duration, current, total,
                                  remaining, percent, bar)
                    if run_once:
                        final_state = "WAITING_FOR_AGENT"
                        break
                    stop_requested.wait(poll_interval)
                    continue
                elif outcome == "cancel_failed":
                    restore_task_front(tasks_file, raw_task)
                    final_state = "CANCEL_FAILED"
                    update_status(final_state, f"{task_id} (execution may still be active)", duration,
                                  current, total, remaining, percent, bar)
                    append_line(progress_file,
                                f"[RECOVERY_HALTED] {task_id}: execution stop was not confirmed; task requeued without restoring workspace.")
                    if run_once:
                        break
                    recovery_halted_task = task_id
                    stop_requested.wait(poll_interval)
                    continue
                elif outcome == "timeout":
                    print(f"[{stamp_now}] [TIMEOUT] Task exceeded {timeout}s limit!")
                    record_task_score("hard_failure", f"timeout after {timeout}s")
                    archive = recover_current_attempt()
                    if archive is None:
                        if run_once:
                            break
                        recovery_halted_task = task_id
                        stop_requested.wait(poll_interval)
                        continue
                    if archive and Path(archive).is_dir():
                        diag_content = (f"# Failure Diagnostic: {task_id}\n\n"
                                        f"- Time: {stamp_now}\n"
                                        f"- Status: TIMEOUT\n"
                                        f"- Timeout Limit: {timeout}s\n"
                                        "- Suggestion: Task may be too complex or model got stuck in large context/compaction. Split into smaller tickets.\n")
                        atomic_write(Path(archive) / "DIAGNOSTIC.md", diag_content)
                    archive_note = f"Report: {Path(archive).name}/DIAGNOSTIC.md\n" if archive else ""
                    summary = ("=" * 80 + f"\n[BLOCKED] {task_id}: Timeout Exceeded ({timeout}s)\n"
                               f"Time: {stamp_now} | Status: TIMEOUT\nFailure Reason: Task execution timed out after {timeout} seconds.\n"
                               + archive_note +
                               "Action Needed for Human: Task may be too complex. Split into smaller atomic tasks.\n" + "=" * 80)
                    prepend_progress(progress_file, summary)
                    append_line(done_file, f"[{stamp_now}] [FAILED] {task} [TIMEOUT]")
                    update_status("BLOCKED", f"{task} (TIMEOUT)", duration, current, total,
                                  max(0, remaining - 1), percent, bar)
                elif outcome in ("step_budget", "closeout_failed"):
                    reason = ("Adapter action budget exhausted" if outcome == "step_budget"
                              else "Restricted ghost closeout failed or attempted implementation")
                    print(f"[{stamp_now}] [HARD FAILURE] {task_id}: {reason}")
                    record_task_score("hard_failure", reason)
                    archive = recover_current_attempt()
                    if archive is None:
                        if run_once:
                            break
                        recovery_halted_task = task_id
                        stop_requested.wait(poll_interval)
                        continue
                    diagnostic_text = payload.get("diagnostic", "") if isinstance(payload, dict) else ""
                    if archive and Path(archive).is_dir():
                        diag_content = (f"# Failure Diagnostic: {task_id}\n\n"
                                        f"- Time: {stamp_now}\n"
                                        f"- Outcome: {outcome}\n"
                                        f"- Reason: {reason}\n")
                        if diagnostic_text:
                            diag_content += f"- Diagnostic:\n{diagnostic_text}\n"
                        if isinstance(payload, dict) and payload.get("verification_evidence"):
                            diag_content += f"- Verification Evidence: {payload.get('verification_evidence')}\n"
                        atomic_write(Path(archive) / "DIAGNOSTIC.md", diag_content)
                    short_diag = f" | Diagnostic: {diagnostic_text[:120]}" if diagnostic_text else ""
                    archive_note = f"\n  Archive: {Path(archive).name}/DIAGNOSTIC.md" if archive else ""
                    prepend_progress(progress_file,
                                     f"[HARD_FAILURE] {task_id}: {reason}{short_diag}; score now {read_score_state(score_state_path)['score']}/5{archive_note}\n"
                                     f"{'[NEED_DECOMPOSITION]' if outcome == 'step_budget' else '[PLANNER_REQUIRED]'} {task_id}: "
                                     "Review the failed-task snapshot and return a bounded ticket through tasks.txt; daemon remains alive.")
                    append_line(done_file, f"[{stamp_now}] [FAILED] {task} [{outcome.upper()}]")
                    update_status("BLOCKED", f"{task} ({outcome.upper()})", duration, current,
                                  total, max(0, remaining - 1), percent, bar)
                elif outcome == "scope":
                    print(f"[{stamp_now}] 🚧 [SCOPE BLOCKED] Worker honestly stopped at unannounced downstream boundaries.")
                    if recover_current_attempt() is None:
                        if run_once:
                            break
                        recovery_halted_task = task_id
                        stop_requested.wait(poll_interval)
                        continue
                    summary = ("=" * 80 + f"\n[SCOPE_BLOCKED] {task_id}: Downstream Scope Boundary Reached\n"
                               f"Time: {stamp_now} | Status: SCOPE_BLOCKED\nDetails: Compiler errors detected in files outside TARGET whitelist.\n"
                               "Worker honestly stopped without violating boundaries. (No credit deduction).\n"
                               "Action Needed for Human: Expand TARGET in tasks.txt or adopt Expand-Migrate-Contract.\n" + "=" * 80)
                    prepend_progress(progress_file, summary)
                    append_line(done_file, f"[{stamp_now}] [SCOPE_BLOCKED] {task}")
                    update_status("SCOPE_BLOCKED", task, duration, current, total,
                                  max(0, remaining - 1), percent, bar)
                elif outcome == "guidance":
                    print(f"[{stamp_now}] ✋ [AWAITING GUIDANCE] Worker honestly requested manager guidance.")
                    if recover_current_attempt() is None:
                        if run_once:
                            break
                        recovery_halted_task = task_id
                        stop_requested.wait(poll_interval)
                        continue
                    append_line(done_file, f"[{stamp_now}] [GUIDANCE_REQUESTED] {task}")
                    update_status("AWAITING_GUIDANCE", task, duration, current, total,
                                  max(0, remaining - 1), percent, bar)
                elif outcome == "blocked":
                    print(f"[{stamp_now}] 🚧 [WORKER BLOCKED] Worker reported [BLOCKED]; rejecting task success.")
                    archive = recover_current_attempt()
                    if archive is None:
                        break
                    diagnostic_text = payload.get("diagnostic", "") if isinstance(payload, dict) else ""
                    if archive and Path(archive).is_dir():
                        diag_content = (f"# Failure Diagnostic: {task_id}\n\n"
                                        f"- Time: {stamp_now}\n"
                                        f"- Status: BLOCKED\n"
                                        f"- Blocker: {diagnostic_text or 'Worker reported [BLOCKED]'}\n")
                        atomic_write(Path(archive) / "DIAGNOSTIC.md", diag_content)
                    archive_note = f"Report: {Path(archive).name}/DIAGNOSTIC.md\n" if archive else ""
                    summary = ("=" * 80 + f"\n[BLOCKED] {task_id}: Worker Reported Blocked\n"
                               f"Time: {stamp_now} | Status: BLOCKED\n"
                               f"Failure Reason: {diagnostic_text or 'Worker final response contained [BLOCKED].'}\n"
                               + archive_note
                               + "Action Needed for Human: Inspect the worker response and resolve its blocker.\n"
                               + "=" * 80)
                    prepend_progress(progress_file, summary)
                    append_line(done_file, f"[{stamp_now}] [FAILED] {task} [WORKER_BLOCKED]")
                    final_state = "BLOCKED"
                    update_status("BLOCKED", f"{task} (WORKER BLOCKED)", duration, current, total,
                                  max(0, remaining - 1), percent, bar)
                elif outcome == "error":
                    print(f"[{stamp_now}] [ERROR] Task execution exited with code {exit_code}")
                    if cli_args.recovery_failed:
                        if not cli_args.recovery_halted:
                            restore_task_front(tasks_file, raw_task)
                            append_line(progress_file,
                                        f"[RECOVERY_HALTED] {task_id}: attempt baseline was not captured; task requeued.")
                            final_state = "RECOVERY_FAILED"
                            update_status(final_state, f"{task_id} (baseline missing)", duration,
                                          current, total, remaining, percent, bar)
                        if run_once:
                            break
                        recovery_halted_task = task_id
                        stop_requested.wait(poll_interval)
                        continue
                    if recover_current_attempt() is None:
                        if run_once:
                            break
                        recovery_halted_task = task_id
                        stop_requested.wait(poll_interval)
                        continue
                    if cli_exit_code is not None and not failure_reason:
                        failure_reason = ("Codex final result format invalid" if result_error_code == "invalid_result"
                                          else f"Codex CLI exited with code {cli_exit_code}" if cli_exit_code
                                          else "Codex reported task failure")
                    codex_details = (f"CLI Exit Code: {cli_exit_code}\nVerification: not run\n"
                                     f"Result Error: {result_error_code or 'none'}\nTask Status: FAILED\n"
                                     if cli_exit_code is not None else "")
                    summary = ("=" * 80 + f"\n[BLOCKED] {task_id}: Execution Failed (Task Exit Code: {exit_code})\n"
                               f"Time: {stamp_now} | Status: FAILED\n{codex_details}"
                               f"Failure Reason: {failure_reason or f'Task execution returned status {exit_code}.'}\n"
                               "Action Needed for Human: Inspect .worker.log for error details.\n" + "=" * 80)
                    prepend_progress(progress_file, summary)
                    append_line(done_file, f"[{stamp_now}] [FAILED] {task} [EXIT_{exit_code}]")
                    update_status("BLOCKED", f"{task} (ERROR {exit_code})", duration, current, total,
                                  max(0, remaining - 1), percent, bar)
                else:
                    audit_args = argparse.Namespace(
                        work_dir=str(work_dir), root=str(root), base=base, target=target,
                        constraints=constraints, title=task, level=int(level or 1),
                        task_id=task_id, verify=verify, port=worker_port, verify_timeout=180,
                        baseline_snapshot=getattr(cli_args, "attempt_baseline", ""),
                    )
                    audit = mechanical_audit(audit_args)
                    if audit["status"] == 0 and is_research:
                        try:
                            publish_research_reports(root, work_dir, target)
                        except (OSError, ValueError, shutil.Error) as error:
                            audit["status"] = 14
                            audit["error"] = f"Research Artifact Publish Failed: {error}"
                    for message in audit["messages"]:
                        print(message)
                    guard_status, guard_error = audit["status"], audit["error"]
                    if guard_status:
                        pending_planner = (task_id, "GUARD_VIOLATION")
                        print(f"[{stamp()}] 🛑 [GUARD REJECTION] Post-flight validation failed: {guard_error}")
                        if recover_current_attempt() is None:
                            if run_once:
                                break
                            recovery_halted_task = task_id
                            stop_requested.wait(poll_interval)
                            continue
                        summary = (f"[MECHANICAL_AUDIT] Status: FAIL | Verify_Exit: {audit['verify_exit']} | Whitelist: {guard_status} | Tree: DIRTY\n"
                                   + "=" * 80 + f"\n[BLOCKED] {task_id}: Guard Validation Failed\n"
                                   f"Time: {stamp()} | Status: GUARD_VIOLATION\nFailure Reason: {guard_error}\n"
                                   "Action Needed for Human: Inspect error and check target whitelist, forbidden shortcuts, or test assertions.\n"
                                   + "=" * 80)
                        prepend_progress(progress_file, summary)
                        append_line(done_file, f"[{stamp()}] [FAILED] {task} [GUARD_VIOLATION]")
                        update_status("BLOCKED", f"{task} (GUARD VIOLATION)", duration,
                                      current, total, max(0, remaining - 1), percent, bar)
                    else:
                        structured_attempts = payload.get("attempts") if isinstance(payload, dict) else None
                        if isinstance(structured_attempts, int) and not isinstance(structured_attempts, bool) and structured_attempts > 0:
                            attempts = str(structured_attempts)
                        else:
                            attempts_match = re.search(r"Attempts:\s*(\d+)", result_text)
                            if not attempts_match and progress_file.exists():
                                attempts_match = re.search(r"Attempts:\s*(\d+)", progress_file.read_text(encoding="utf-8", errors="replace"))
                            attempts = attempts_match.group(1) if attempts_match else "1"
                        diff = subprocess.run(["git", "-C", str(work_dir), "diff", "--stat", base, "HEAD"],
                                               capture_output=True, text=True, check=False).stdout.splitlines()
                        diff_stat = diff[-1].strip() if diff else "0 files changed"
                        audit_ns = time.time_ns()
                        prepend_progress(progress_file, f"[MECHANICAL_AUDIT] Status: PASS | Task: {task_id} | Verify_Exit: 0 | Tests_Run: >=1 | Whitelist: PASS | Tree: CLEAN | Diff: {diff_stat} | Attempts: {attempts} | Audit_NS: {audit_ns} | Planner_Review: pending")
                        # Audit passed: retire agent-marked reports, then validate remaining evidence.
                        settle_invalid_reports(root, work_dir, accepted=True)
                        refresh_active_reports(root, work_dir)
                        if root.resolve() != work_dir.resolve():
                            settle_invalid_reports(work_dir, work_dir, accepted=True)
                            refresh_active_reports(work_dir, work_dir)
                        record_task_score("success")
                        new_done = done_count + 1
                        final_bar = progress_bar(new_done, total)
                        print(f"[{stamp()}] [SUCCESS] Task completed in {duration}s | {final_bar}")
                        append_line(done_file, f"[{stamp()}] [DONE] {task} (Duration: {duration}s) [Audit_NS: {audit_ns}]")
                        update_status("IDLE", current=new_done, total=total,
                                      remaining=max(0, remaining - 1),
                                      percentage=f"{new_done * 100 // total if total else 0}%", bar=final_bar)
            stop_requested.wait(poll_interval)
        return 0
    finally:
        heartbeat_stop.set()
        if heartbeat.is_alive():
            heartbeat.join(timeout=2)
        try:
            pid_file.unlink(missing_ok=True)
        except OSError:
            pass
        try:
            update_status(final_state, bar="[------------------------------] 0%")
        except (OSError, ValueError):
            pass


def git_audit(args):
    """Return the existing post-flight guard code and a human-readable reason."""
    def git(*command):
        return subprocess.run(["git", "-C", args.work_dir, *command],
                              check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout

    trees = attempt_audit_trees(args)
    ignored = re.compile(r"(?:^|/)(?:nightshift/)?(?:progress\.log|tasks\.txt|tasks\.done|worker\..*\.status|\.preview|\.reports|\.worker.*)")
    if trees:
        changed = changed_tree_paths(args.work_dir, *trees)
        dirty = set(changed_tree_paths(args.work_dir, "HEAD", trees[1])).intersection(changed)
        dirty = [name for name in dirty if not ignored.search(name)]
        diff_base = list(trees)
    else:
        status = git("status", "--porcelain", "--untracked-files=all", "-z")
        dirty = []
        for entry in status.split(b"\0"):
            if not entry:
                continue
            path = entry.decode("utf-8", "replace")
            # Porcelain status prefixes ("?? ", " M ", etc.) are not part of
            # the file path patterns used for runtime-owned artifacts.
            path = path[3:] if len(path) >= 3 else path
            if not ignored.search(path):
                dirty.append(path)
        changed = [name.decode("utf-8", "replace") for name in
                   git("diff", "--name-only", "-z", args.base).split(b"\0") if name]
        if getattr(args, "allow_dirty", False):
            changed.extend(name.decode("utf-8", "replace") for name in
                           git("ls-files", "--others", "--exclude-standard", "-z").split(b"\0") if name)
        diff_base = [args.base]
    if dirty and not getattr(args, "allow_dirty", False):
        return 1, "Dirty working tree or untracked ghost files detected after task. " + ", ".join(dirty)

    diagnostic = task_header_has_tag(args.title, r"\[DIAGNOSTIC\]")
    research = is_research_task(args.title)
    if diagnostic and any(re.match(r"^(Sources|src|app|lib)/", name, re.I) for name in changed):
        return 4, "Diagnostic Violation: Modifying production code in [DIAGNOSTIC] is strictly forbidden. Only tests and .reports/ allowed."

    if not diagnostic and not research:
        tests = [name for name in changed if re.search(r"(^tests/|^Tests/|^test/|Test\.[a-z]+$|_test\.[a-z]+$)", name, re.I)]
        if tests:
            unauthorized = (task_header_has_tag(args.title, r"\[CODE:\s*FIX\]")
                            or not re.search(r"tests/|Tests/|test", args.target, re.I))
            if unauthorized and not re.search(r"test refactor approved", args.constraints, re.I):
                return 5, f"Test Immutability Violation: Modified test files without authorization in TARGET ({', '.join(tests)})."
            diff = git("diff", *diff_base, "--", *tests).decode("utf-8", "replace")
            assertion = re.compile(r"\s*(XCTAssert|#expect|assert|expect\(|self\.assert)")
            deleted = sum(bool(assertion.match(line[1:])) for line in diff.splitlines()
                          if line.startswith("-") and not line.startswith("---"))
            added = sum(bool(assertion.match(line[1:])) for line in diff.splitlines()
                        if line.startswith("+") and not line.startswith("+++"))
            if deleted > added:
                return 7, f"Assertion Non-Decrease Violation: Deleted {deleted} assertions while adding only {added}."

    numstat = git("diff", "--numstat", *diff_base).decode("utf-8", "replace")
    deleted_lines = 0
    for line in numstat.splitlines():
        parts = line.split("\t")
        if (len(parts) >= 3 and parts[1].isdigit()
                and not parts[2].startswith(".reports/")):
            deleted_lines += int(parts[1])
    if not re.search(r"large deletion approved|unrestricted", args.constraints, re.I):
        limit = {2: 150, 3: 40}.get(args.level)
        if limit is not None and deleted_lines > limit:
            return 8, f"Blast Radius Violation: Level {args.level} tasks cannot delete >{limit} lines (Deleted: {deleted_lines})."

    if args.target and not re.search(r"unrestricted|multi-interface", args.constraints, re.I) and not (diagnostic or research):
        targets = [item.strip().rstrip("/").casefold() for item in args.target.split(",") if item.strip()]
        exempt = {"progress.log", "tasks.txt", "tasks.done", "worker.status", ".gitignore", ".worker.notes.md",
                  "nightshift/progress.log", "nightshift/tasks.txt", "nightshift/tasks.done",
                  "nightshift/worker.status", "nightshift/.worker.notes.md"}
        for name in changed:
            if (name in exempt or name.startswith((".reports/", "docs/reports/", ".worker-ghosts/",
                                                   "nightshift/.worker-ghosts/"))
                    or re.fullmatch(r"(?:nightshift/)?\.worker-ghost-state(?:\.[A-Za-z0-9_.-]+)?\.json", name)):
                continue
            lowered = name.casefold()
            if not any(lowered == target or lowered.startswith(target + "/") for target in targets):
                return 2, f"Target Whitelist Violation: Modified unannounced file '{name}' outside declared scope."
    return 0, ""


def run_verify(command, work_dir, port, timeout):
    environment = os.environ.copy()
    environment.update(PORT=str(port), TEST_PORT=str(port))
    options = {"cwd": work_dir, "env": environment, "stdout": subprocess.PIPE,
               "stderr": subprocess.STDOUT}
    if os.name == "nt":
        options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        options["start_new_session"] = True
    process = subprocess.Popen(["bash", "-c", command], **options)
    try:
        output, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            if os.name == "nt":
                process.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                os.killpg(process.pid, signal.SIGTERM)
        except (OSError, ValueError):
            process.terminate()
        try:
            output, _ = process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            if os.name == "nt":
                process.kill()
            else:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except OSError:
                    process.kill()
            try:
                output, _ = process.communicate(timeout=0.5)
            except subprocess.TimeoutExpired as error:
                output = error.output or b""
                for stream in (process.stdout, process.stderr):
                    if stream is not None:
                        stream.close()
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired as cleanup_error:
                    raise RuntimeError("VERIFY cleanup could not reap owned direct child") from cleanup_error
        return 124, output.decode("utf-8", "replace")
    else:
        return process.returncode, output.decode("utf-8", "replace")


def first_report(root, target):
    root = Path(root).resolve()
    reports = (root / ".reports").resolve()
    for declared in (part.strip() for part in (target or "").split(",")):
        if not declared:
            continue
        candidate = (root / declared).resolve()
        if not candidate.is_relative_to(reports) or candidate == reports:
            continue
        if candidate.is_file():
            if candidate.name == "SUMMARY.md":
                return candidate
        elif candidate.is_dir():
            for current, directories, files in os.walk(candidate, followlinks=False):
                directories[:] = [name for name in directories if not name.startswith(".")]
                if "SUMMARY.md" in files:
                    return Path(current) / "SUMMARY.md"
    return None


def publish_research_reports(root, work_dir, target):
    """Copy declared report artifacts when the worker uses a separate tree."""
    root, work_dir = Path(root).resolve(), Path(work_dir).resolve()
    if root == work_dir:
        return
    source_reports = (work_dir / ".reports").resolve()
    for declared in (part.strip() for part in (target or "").split(",")):
        if not declared:
            continue
        relative = Path(declared)
        if relative.is_absolute() or not relative.parts or relative.parts[0] != ".reports":
            continue
        source = (work_dir / relative).resolve()
        if not source.is_relative_to(source_reports):
            raise ValueError(f"declared report escapes .reports/: {declared}")
        if not source.exists():
            continue
        destination = root / relative
        if source.is_dir():
            for current, directories, files in os.walk(source, followlinks=False):
                directories[:] = [name for name in directories if not name.startswith(".")]
                relative_dir = Path(current).relative_to(work_dir)
                for filename in files:
                    source_file = Path(current) / filename
                    if source_file.is_symlink():
                        raise ValueError(f"report artifact cannot be a symlink: {source_file}")
                    target_file = root / relative_dir / filename
                    target_file.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source_file, target_file)
        elif source.is_file():
            if source.is_symlink():
                raise ValueError(f"report artifact cannot be a symlink: {source}")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)


def mechanical_audit(args):
    guard_status, reason = git_audit(args)
    verify_exit = 0
    messages = []
    verify_evidence = []

    def run_with_evidence(command, stage="verify"):
        code, output = run_verify(command, args.work_dir, args.port, args.verify_timeout)
        verify_evidence.append({"stage": stage, "command": command, "exit_code": code,
                                **bounded_verify_evidence(output)})
        return code, output

    if guard_status:
        return {"status": guard_status, "error": reason, "verify_exit": verify_exit,
                "messages": messages, "verify_evidence": verify_evidence}

    diagnostic = task_header_has_tag(args.title, r"\[DIAGNOSTIC\]")
    research = is_research_task(args.title)
    verify = args.verify

    if diagnostic:
        messages.append(f"[WORKER_GUARD] 🔍 Executing Mechanical Audit ([DIAGNOSTIC]): {verify or 'None'}")
        if verify:
            verify_exit, output = run_with_evidence(verify)
            if re.search(r"error: cannot find|syntax error|compilation failed|build failed|undefined symbol",
                         output, re.I):
                guard_status = 4
                reason = "False Red-Light Detected: Repro failed on compilation/syntax error instead of test assertion."
    elif research:
        messages.append("[WORKER_GUARD] 🔍 Executing Mechanical Audit ([RESEARCH])...")
        trees = attempt_audit_trees(args)
        if trees:
            # Use the same captured baseline as git_audit: existing user work is
            # not a research edit, while a new product commit still is one.
            changed = changed_tree_paths(args.work_dir, *trees)
        else:
            status = subprocess.run(["git", "-C", args.work_dir, "status", "--porcelain"],
                                    check=False, capture_output=True, text=True).stdout
            changed = status.splitlines()
        dirty = [line for line in changed if not re.search(
            r"progress\.log|tasks\.txt|tasks\.done|worker\..*\.status|\.preview|\.worker.*|\.reports/|\.reports", line)]
        if dirty:
            guard_status = 12
            reason = "Research Zero-Touch Violation: Uncommitted/modified files detected after research (" + "\n".join(dirty) + ")."
        report = first_report(args.work_dir, args.target) if guard_status == 0 else None
        if guard_status == 0:
            if report is None or not report.is_file() or report.stat().st_size == 0:
                guard_status = 11
                reason = "Research Artifact Missing: No non-empty SUMMARY.md found in .reports/ or declared TARGET."
            else:
                content = report.read_text(encoding="utf-8", errors="replace")
                if not re.search(r"^#+\s+", content, re.M):
                    guard_status = 11
                    reason = f"Research Artifact Invalid: Report '{report}' lacks standard markdown headings."
                else:
                    metadata = re.search(r"REPORT_META:\s*Task=([^ |]+)", content.splitlines()[0])
                    if metadata and metadata.group(1) != args.task_id:
                        guard_status = 11
                        reason = (f"Research Artifact Stale: Report '{report}' belongs to task "
                                  f"{metadata.group(1)}, expected {args.task_id}.")
                    elif not metadata:
                        commit = subprocess.run(["git", "-C", args.work_dir, "rev-parse", "--short", "HEAD"],
                                                check=False, capture_output=True, text=True).stdout.strip() or "HEAD"
                        stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
                        atomic_write(report, f"<!-- REPORT_META: Task={args.task_id} | Commit={commit} | Time={stamp} -->\n{content}")
                        content = report.read_text(encoding="utf-8", errors="replace")
                    if guard_status == 0:
                        messages.append(f"[WORKER_GUARD] ✅ Artifact verified: {report} ({report.stat().st_size} bytes)")
                        pointers = re.findall(r"[a-zA-Z0-9_./-]+\.[a-zA-Z0-9]+#L[0-9]+", content)[:5]
                        for pointer in pointers:
                            file_name, line_number = pointer.rsplit("#L", 1)
                            source = Path(args.work_dir) / file_name
                            if source.is_file():
                                total_lines = source.read_bytes().count(b"\n")
                                if int(line_number) > total_lines:
                                    guard_status = 13
                                    reason = (f"Pointer Validity Violation: Line {line_number} in pointer '{pointer}' "
                                              f"exceeds total file lines ({total_lines}).")
                                    break
                                messages.append(f"[WORKER_GUARD] 🔍 Verified pointer target: {file_name}#L{line_number} (Total lines: {total_lines})")
        if guard_status == 0 and verify and not re.match(r"^(REPORT_ONLY|none)$", verify, re.I):
            messages.append(f"[WORKER_GUARD] 🔍 Executing Custom Research VERIFY: {verify}")
            verify_exit, _ = run_with_evidence(verify)
            if verify_exit:
                guard_status = 6
                reason = f"Mechanical Verification Failed: '{verify}' returned exit code {verify_exit} (Expected 0)."
    else:
        messages.append(f"[WORKER_GUARD] 🔍 Executing Mechanical Audit ([CODE]): {verify or 'None'}")
        if verify:
            verify_exit, output = run_with_evidence(verify)
            if verify_exit:
                guard_status = 6
                reason = f"Mechanical Verification Failed: '{verify}' returned exit code {verify_exit} (Expected 0)."
            elif re.search(r"(0 tests passed|0 passed|ran 0 tests|0 suites passed|No matching tests)", output, re.I):
                guard_status = 9
                reason = "Empty Test Run Detected (Anti-Idle): VERIFY ran 0 tests. Possible test filter typo."
            elif (Path(args.work_dir) / "Package.swift").is_file():
                messages.append("[WORKER_GUARD] 🔍 Executing Regression Check (swift test)...")
                regression, _ = run_with_evidence("swift test", "regression")
                if regression:
                    guard_status = 10
                    reason = "Regression Check Failed: Whole-suite regression tests failed."
    return {"status": guard_status, "error": reason, "verify_exit": verify_exit,
            "messages": messages, "verify_evidence": verify_evidence}


def report_archive_path(root, timestamp, name):
    archive = Path(root) / ".reports" / ".archive"
    archive.mkdir(parents=True, exist_ok=True)
    candidate = archive / f"{timestamp}-{name}"
    suffix = 1
    while candidate.exists():
        candidate = archive / f"{timestamp}-{name}-{suffix}"
        suffix += 1
    return candidate


def cleanup_invalid_reports(root):
    """Delete only entries explicitly quarantined under the runtime-owned .invalid/.

    Agents may move stale report directories here but must not delete them. Never
    follow symlinks (including a symlink substituted for the quarantine root).
    """
    invalid = Path(root) / ".reports" / ".invalid"
    if invalid.is_symlink():
        raise ValueError(f"report quarantine may not be a symlink: {invalid}")
    invalid.mkdir(parents=True, exist_ok=True)
    for entry in invalid.iterdir():
        if entry.is_symlink() or not entry.is_dir():
            entry.unlink()
        else:
            shutil.rmtree(entry)


def settle_invalid_reports(root, work_dir, accepted):
    """Settle agent quarantine only at a safe task boundary.

    Successful audited tasks confirm deletion. Failed attempts first restore their
    code baseline; return still-current reports to active, retire stale reports,
    and never overwrite a conflicting active report.
    """
    root = Path(root)
    invalid = root / ".reports" / ".invalid"
    if invalid.is_symlink():
        raise ValueError(f"report quarantine may not be a symlink: {invalid}")
    if not invalid.exists():
        return
    if accepted:
        cleanup_invalid_reports(root)
        return
    for entry in sorted(invalid.iterdir()):
        if entry.is_symlink() or not entry.is_dir():
            # Discard only the entry itself, never follow a symlink.
            entry.unlink()
            continue
        destination = root / ".reports" / entry.name
        reason = report_staleness(entry, work_dir)
        if reason or destination.exists() or destination.is_symlink():
            timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
            archived = report_archive_path(root, timestamp, entry.name)
            shutil.move(str(entry), str(archived))
            print(f"[WORKER] Archived quarantined report '{entry.name}': "
                  f"{reason or 'active report name already exists'}")
        else:
            shutil.move(str(entry), str(destination))
            print(f"[WORKER] Restored valid report '{entry.name}' after rollback")


def report_source_paths(report):
    """Read source citations written by research agents, excluding report files."""
    files = [report / "SUMMARY.md"] if report.is_dir() else [report]
    if report.is_dir():
        files.extend(path for path in report.rglob("*.md")
                     if path.name != "SUMMARY.md" and not path.is_symlink()
                     and not any(part.startswith(".") for part in path.relative_to(report).parts))
    paths = set()
    for document in files:
        if not document.is_file() or document.is_symlink():
            continue
        body = document.read_text(encoding="utf-8", errors="replace")
        for raw in re.findall(r"(?<![\w./-])([A-Za-z0-9_./+-]+\.[A-Za-z0-9]+)#L[0-9]+", body):
            path = Path(raw)
            if (not path.is_absolute() and ".." not in path.parts
                    and path.parts[0] != ".reports"):
                paths.add(path.as_posix())
    return sorted(paths)


def report_staleness(report, work_dir):
    """Return a reason for stale/unverifiable reports, or None when sources match.

    Freshness is checked from each report's own REPORT_META commit, not merely
    the previous worker task's diff. Local staged/unstaged changes count.
    """
    summary = report / "SUMMARY.md" if report.is_dir() else report
    if not summary.is_file() or summary.is_symlink():
        return "missing SUMMARY.md"
    with summary.open(encoding="utf-8", errors="replace") as stream:
        first_line = stream.readline()
    match = re.search(r"REPORT_META:.*?\bCommit=([0-9a-fA-F]{7,40})\b", first_line)
    if not match:
        return "missing valid REPORT_META commit"
    commit = match.group(1)
    paths = report_source_paths(report)
    if not paths:
        return "no verifiable source path#Lline citations"

    def git(*args):
        return subprocess.run(["git", "-C", str(work_dir), *args],
                              check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    if git("cat-file", "-e", f"{commit}^{{commit}}").returncode:
        return f"unknown source commit {commit}"
    if git("merge-base", "--is-ancestor", commit, "HEAD").returncode:
        return f"source commit {commit} not in current history"
    tracked = set(git("ls-files", "--cached", "--", *paths).stdout.decode("utf-8", "replace").splitlines())
    if len(tracked) != len(paths) or any(not (Path(work_dir) / path).is_file() for path in paths):
        return "referenced source missing or untracked"
    if git("diff", "--quiet", commit, "--", *paths).returncode:
        return "referenced source changed since report commit"
    return None


def refresh_active_reports(root, work_dir):
    """Script-only classification: archive stale reports, preserve all valid ones."""
    reports = Path(root) / ".reports"
    reports.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    for entry in reports.iterdir():
        if entry.name.startswith("."):
            continue
        if entry.is_symlink():
            continue  # Never follow or move an untrusted link.
        if not entry.is_dir() and entry.name != "SUMMARY.md":
            continue
        reason = report_staleness(entry, work_dir)
        if reason:
            shutil.move(str(entry), str(report_archive_path(root, timestamp, entry.name)))
            print(f"[WORKER] 📦 Archived stale report '{entry.name}': {reason}")


def archive_active_reports(root):
    """Explicit force-archive command (not used for routine research dispatch)."""
    reports = Path(root) / ".reports"
    reports.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    for entry in reports.iterdir():
        if entry.name.startswith("."):
            continue
        if entry.is_symlink():
            continue
        shutil.move(str(entry), str(report_archive_path(root, timestamp, entry.name)))


def archive_overlapping_reports(root, work_dir, base):
    """Compatibility entry point; validate against each report's own source commit."""
    refresh_active_reports(root, work_dir)


def _git_path_state(path):
    path = Path(path)
    if path.is_symlink():
        return {"kind": "symlink", "target": os.readlink(path),
                "mode": path.lstat().st_mode & 0o777}
    if path.is_file():
        return {"kind": "file", "mode": path.stat().st_mode & 0o777}
    return {"kind": "missing"}


def _runtime_state_root(root):
    root = Path(root)
    return root / "nightshift" if (root / "nightshift").is_dir() else root


def _scratch_state_root(root):
    """Keep Worker attempt and recovery artifacts inside the installed bundle."""
    return _runtime_state_root(root) / ".scratch"


def _git_names(work_dir, *arguments):
    result = subprocess.run(["git", "-C", str(work_dir), *arguments],
                            check=True, capture_output=True)
    return [os.fsdecode(name) for name in result.stdout.split(b"\0") if name]


@serialize_workspace_state
def capture_attempt_baseline(work_dir, task_id, attempt_id, snapshot_path):
    """Capture the exact Git state at the seam before an execution attempt using pure Git."""
    work_dir, snapshot_path = Path(work_dir).resolve(), Path(snapshot_path).resolve()
    if snapshot_path.exists():
        raise FileExistsError(f"attempt snapshot already exists: {snapshot_path}")
    head = subprocess.run(["git", "-C", str(work_dir), "rev-parse", "HEAD"],
                          check=True, capture_output=True, text=True).stdout.strip()
    untracked = _git_names(work_dir, "ls-files", "--others", "--exclude-standard", "-z")
    stash = subprocess.run(["git", "-C", str(work_dir), "stash", "create"],
                           check=False, capture_output=True, text=True).stdout.strip()
    snapshot_path.mkdir(parents=True)
    atomic_write(snapshot_path / "snapshot.json", json.dumps({
        "version": 1, "task_id": task_id, "attempt_id": attempt_id,
        "work_dir": str(work_dir), "head": head,
        "stash": stash,
        "untracked": untracked,
        "audit_tree": worktree_tree(work_dir),
        "index_changes": _git_names(work_dir, "diff", "--cached", "--name-only", "-z", head),
        "paths": {},
    }, ensure_ascii=False, indent=2) + "\n")
    return snapshot_path


@serialize_workspace_state
def recover_attempt_baseline(work_dir, snapshot_path, archive_path, runtime_paths=()):
    """Archive a failed attempt's diff and restore Git state via pure Git."""
    work_dir, snapshot_path, archive_path = map(Path, (work_dir, snapshot_path, archive_path))
    work_dir, snapshot_path, archive_path = work_dir.resolve(), snapshot_path.resolve(), archive_path.resolve()
    metadata_path = snapshot_path / "snapshot.json"
    if not metadata_path.is_file():
        raise ValueError(f"attempt snapshot is missing metadata: {metadata_path}")
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if metadata.get("version") != 1:
        raise ValueError(f"invalid attempt snapshot: {metadata_path}")
    baseline_head = metadata.get("head", "")
    valid_head = subprocess.run(["git", "-C", str(work_dir), "cat-file", "-e", f"{baseline_head}^{{commit}}"],
                                check=False, capture_output=True)
    if valid_head.returncode:
        raise ValueError(f"attempt baseline commit is unavailable: {baseline_head}")
    if archive_path.exists():
        raise FileExistsError(f"attempt archive already exists: {archive_path}")

    archive_path.mkdir(parents=True)
    diff = subprocess.run(["git", "-C", str(work_dir), "diff", "--binary", baseline_head],
                          check=True, capture_output=True).stdout
    (archive_path / "changes.diff").write_bytes(diff)
    status = subprocess.run(["git", "-C", str(work_dir), "status", "--porcelain", "-z"],
                            check=True, capture_output=True).stdout
    (archive_path / "status.txt").write_bytes(status)
    commits = subprocess.run(["git", "-C", str(work_dir), "log", "--format=%H %s",
                              f"{baseline_head}..HEAD"], check=True,
                             capture_output=True, text=True).stdout
    atomic_write(archive_path / "commits.txt", commits)

    # In-memory preservation of runtime status files (e.g. tasks.txt, progress.log, status)
    runtime_states = {}
    for raw_path in runtime_paths:
        candidate = Path(raw_path)
        if not candidate.is_absolute():
            candidate = work_dir / candidate
        if candidate.is_file():
            runtime_states[str(candidate.resolve())] = candidate.read_bytes()

    # Pure Git restore:
    # 1. Reset all tracked files and index to baseline_head
    reset = subprocess.run(["git", "-C", str(work_dir), "reset", "--hard", baseline_head],
                           check=False, capture_output=True)
    if reset.returncode:
        raise RuntimeError("could not reset failed attempt commits: " +
                           reset.stderr.decode("utf-8", "replace"))

    # 2. Clean only newly introduced untracked files (preserving baseline untracked & runtime)
    baseline_untracked = set(metadata.get("untracked", []))
    runtime_resolved = set(runtime_states)
    for relative in _git_names(work_dir, "ls-files", "--others", "--exclude-standard", "-z"):
        if relative in baseline_untracked or relative.startswith((".scratch/", "nightshift/.scratch/", ".worker-ghosts/", "nightshift/.worker-ghosts/")):
            continue
        p = work_dir / relative
        if str(p.resolve()) in runtime_resolved:
            continue
        if p.is_file() or p.is_symlink():
            p.unlink()
        elif p.is_dir():
            shutil.rmtree(p)

    # 3. Apply baseline stash if one was created at attempt start
    stash = metadata.get("stash", "")
    if stash:
        subprocess.run(["git", "-C", str(work_dir), "stash", "apply", "--index", stash],
                       check=False, capture_output=True)

    # 4. Restore in-memory runtime files
    for path_str, content in runtime_states.items():
        Path(path_str).write_bytes(content)

    atomic_write(archive_path / "recovery.json", json.dumps({
        "task_id": metadata.get("task_id", ""), "attempt_id": metadata.get("attempt_id", ""),
        "baseline_head": baseline_head, "recovered": True,
    }, ensure_ascii=False, indent=2) + "\n")
    return archive_path


def rollback_failed_task(root, work_dir, task_id, runtime_paths, baseline_snapshot=None,
                         attempt_id=""):
    if baseline_snapshot:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        base = _scratch_state_root(root) / "failed_tasks" / f"{task_id}_{attempt_id or stamp}"
        archive = base
        suffix = 1
        while archive.exists():
            archive = Path(f"{base}_{suffix}")
            suffix += 1
        return recover_attempt_baseline(work_dir, baseline_snapshot, archive, runtime_paths)
    root, work_dir = Path(root).resolve(), Path(work_dir).resolve()
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup = _scratch_state_root(root) / "failed_tasks" / f"{task_id}_{stamp}"
    suffix = 1
    while backup.exists():
        backup = _scratch_state_root(root) / "failed_tasks" / f"{task_id}_{stamp}_{suffix}"
        suffix += 1
    backup.mkdir(parents=True)
    print(f"[WORKER] Isolating failed changes to {backup} and resetting clean git tree...",
          file=sys.stderr)

    diff = subprocess.run(["git", "-C", str(work_dir), "diff"],
                          check=False, capture_output=True)
    (backup / "changes.diff").write_bytes(diff.stdout)
    status = subprocess.run(["git", "-C", str(work_dir), "status", "--porcelain"],
                            check=False, capture_output=True)
    (backup / "status.txt").write_bytes(status.stdout)

    runtime_states = {}
    for raw_path in runtime_paths:
        candidate = Path(raw_path)
        if not candidate.is_absolute():
            candidate = work_dir / candidate
        if candidate.is_file():
            runtime_states[str(candidate.resolve())] = candidate.read_bytes()

    subprocess.run(["git", "-C", str(work_dir), "reset", "--hard", "HEAD"],
                   check=False, capture_output=True)
    subprocess.run(["git", "-C", str(work_dir), "clean", "-fd", "-e", ".scratch", "-e", ".worker*", "-e", "nightshift/"],
                   check=False, capture_output=True)

    for path_str, content in runtime_states.items():
        Path(path_str).write_bytes(content)

    restore_paths = ["."]
    for runtime_path in runtime_paths:
        try:
            candidate = Path(runtime_path)
            if not candidate.is_absolute():
                candidate = work_dir / candidate
            candidate = candidate.parent.resolve() / candidate.name
            relative = candidate.relative_to(work_dir)
        except ValueError:
            continue
        if str(relative) != ".":
            restore_paths.append(f":(exclude){relative.as_posix()}")
    subprocess.run(["git", "-C", str(work_dir), "restore", "--worktree", "--",
                    *restore_paths], check=False, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    return backup


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    score_read = commands.add_parser("score-read")
    score_read.add_argument("--state", required=True)
    score_read.add_argument("--agent-id", default="")
    score_record = commands.add_parser("score-record")
    score_record.add_argument("--state", required=True)
    score_record.add_argument("--task-id", required=True)
    score_record.add_argument("--result", choices=("accept", "reject", "no_fault", "hard_failure", "success"), required=True)
    score_record.add_argument("--evidence", default="")
    score_record.add_argument("--progress-file", default="")
    score_record.add_argument("--audit-ns", default="")
    score_record.add_argument("--agent-id", default="")
    score_record.add_argument("--events", default="")
    score_record.add_argument("--mid-batch", action="store_true")
    score_record.add_argument("--dispatch-gate", default="")
    event_watch = commands.add_parser("agent-events-watch")
    event_watch.add_argument("--events", required=True)
    event_watch.add_argument("--cursor", required=True)
    event_watch.add_argument("--acknowledgements", required=True)
    event_watch.add_argument("--gate", required=True)
    event_watch.add_argument("--handoff", required=True)
    event_watch.add_argument("--replay-pending", action="store_true",
                             help="Replay all unacknowledged events even if already seen in cursor")
    event_ack = commands.add_parser("agent-event-ack")
    event_ack.add_argument("--events", required=True)
    event_ack.add_argument("--acknowledgements", required=True)
    event_ack.add_argument("--event-id", required=True)
    event_ack.add_argument("--resolution", choices=("acknowledged", "revised"), required=True)
    ghost_prompt = commands.add_parser("ghost-prompt")
    ghost_prompt.add_argument("--output", required=True)
    ghost_prompt.add_argument("--task-id", required=True)
    ghost_prompt.add_argument("--verify", default="")
    ghost_prompt.add_argument("--progress-file", default="progress.log")
    pop = commands.add_parser("queue-pop")
    pop.add_argument("path")
    insert = commands.add_parser("queue-insert")
    insert.add_argument("--tasks", required=True)
    insert.add_argument("--task", required=True)
    field = commands.add_parser("task-field")
    field.add_argument("raw")
    field.add_argument("key", choices=FIELD_NAMES)
    status = commands.add_parser("status-write")
    for name in ("path", "worker_id", "os_name", "platform",
                 "work_dir", "state", "current_task"):
        status.add_argument(name)
    for name in ("elapsed", "current_index", "total_tasks", "remaining"):
        status.add_argument(name, type=int)
    for name in ("percentage", "bar"):
        status.add_argument(name)
    status.add_argument("pid", type=int)
    status.add_argument("model")
    status.add_argument("cli")
    status.add_argument("timeout", type=int)
    display = commands.add_parser("status-display")
    display.add_argument("path")
    task_cli = commands.add_parser("task-cli")
    task_cli.add_argument("--adapter", required=True)
    task_cli.add_argument("--executable", required=True)
    task_cli.add_argument("--extra-arg", dest="extra_args", action="append", default=[])
    task_cli.add_argument("--overrides", dest="overrides_json", default="{}")
    task_cli.add_argument("--work-dir", required=True)
    task_cli.add_argument("--prompt-file", required=True)
    task_cli.add_argument("--output-file", required=True)
    task_cli.add_argument("--result-file", default="")
    task_cli.add_argument("--timeout", type=float, default=1800)
    task_cli.add_argument("--step-budget", type=int, default=17)
    task_cli.add_argument("--worker-port", default="3001")
    task_cli.add_argument("--target", default="")
    task_cli.add_argument("--task-id", default="TASK-UNKNOWN")
    task_cli.add_argument("--title", default="")
    task_cli.add_argument("--level", default="1")
    task_cli.add_argument("--verify", default="")
    task_cli.add_argument("--base", default="")
    task_cli.add_argument("--progress-file", default="progress.log")
    task_cli.add_argument("--ghost-state-file", default="")
    task_cli.add_argument("--agent-id", default="")
    task_cli.add_argument("--score-state-file", default="")
    attempt_snapshot = commands.add_parser("attempt-snapshot")
    attempt_snapshot.add_argument("--work-dir", required=True)
    attempt_snapshot.add_argument("--task-id", required=True)
    attempt_snapshot.add_argument("--attempt-id", required=True)
    attempt_snapshot.add_argument("--snapshot", required=True)
    attempt_recover = commands.add_parser("attempt-recover")
    attempt_recover.add_argument("--work-dir", required=True)
    attempt_recover.add_argument("--snapshot", required=True)
    attempt_recover.add_argument("--archive", required=True)
    adapter_exec = commands.add_parser("adapter-exec")
    adapter_exec.add_argument("--command", dest="adapter_command", required=True)
    adapter_exec.add_argument("--adapter-arg", action="append", default=[])
    adapter_exec.add_argument("--request", required=True)
    adapter_exec.add_argument("--trace", default="")
    adapter_exec.add_argument("--callback-timeout", type=float, default=30)
    adapter_exec.add_argument("--execution-timeout", type=float, default=1800)
    timeout = commands.add_parser("run-timeout")
    timeout.add_argument("seconds", type=float)
    timeout.add_argument("argv", nargs=argparse.REMAINDER)
    scratch = commands.add_parser("scratch-review-check")
    scratch.add_argument("--root", required=True)
    scratch.add_argument("--stats-only", action="store_true")
    code_index = commands.add_parser("code-index")
    code_index.add_argument("indexer_args", nargs=argparse.REMAINDER)
    audit = commands.add_parser("git-audit")
    audit.add_argument("--work-dir", required=True)
    audit.add_argument("--baseline-snapshot", default="")
    audit.add_argument("--base", required=True)
    audit.add_argument("--target", default="")
    audit.add_argument("--constraints", default="")
    audit.add_argument("--title", default="")
    audit.add_argument("--level", type=int, default=1)
    archive = commands.add_parser("archive-reports")
    archive.add_argument("--root", required=True)
    refresh = commands.add_parser("refresh-reports")
    refresh.add_argument("--root", required=True)
    refresh.add_argument("--work-dir", required=True)
    overlap = commands.add_parser("archive-overlapping-reports")
    overlap.add_argument("--root", required=True)
    overlap.add_argument("--work-dir", required=True)
    overlap.add_argument("--base", default="HEAD~1")
    rollback = commands.add_parser("rollback-failed-task")
    rollback.add_argument("--root", required=True)
    rollback.add_argument("--work-dir", required=True)
    rollback.add_argument("--task-id", required=True)
    rollback.add_argument("--tasks", required=True)
    rollback.add_argument("--tasks-done", default="")
    rollback.add_argument("--progress", default="")
    progress = commands.add_parser("progress-prepend")
    progress.add_argument("--path", required=True)
    progress.add_argument("--entry", required=True)
    audit = commands.add_parser("mechanical-audit")
    audit.add_argument("--baseline-snapshot", default="")
    for name in ("work-dir", "base", "task-id"):
        audit.add_argument(f"--{name}", required=True)
    for name in ("target", "constraints", "title", "verify"):
        audit.add_argument(f"--{name}", default="")
    audit.add_argument("--level", type=int, default=1)
    audit.add_argument("--port", default="3001")
    audit.add_argument("--verify-timeout", type=float, default=180)
    poll = commands.add_parser("poll-next")
    poll.add_argument("--tasks", required=True)
    poll.add_argument("--done", required=True)
    poll.add_argument("--pause", required=True)
    poll.add_argument("--dispatch-gate", default="")
    poll.add_argument("--work-dir", required=True)
    poll.add_argument("--step-base", type=int, default=12)
    poll.add_argument("--step-per-file", type=int, default=5)
    daemon = commands.add_parser("daemon")
    daemon.add_argument("--root", required=True)
    args = parser.parse_args(argv)
    if args.command == "score-read":
        state = (read_agent_score_state(args.state, args.agent_id) if args.agent_id
                 else read_score_state(args.state))
        print(json.dumps(score_summary(state), ensure_ascii=False))
    elif args.command == "score-record":
        if args.agent_id:
            event_path = args.events or str(Path(args.state).parent / ".worker-agent-events.jsonl")
            result = record_agent_score_result(args.state, args.agent_id, args.task_id,
                                               args.result, args.evidence,
                                               event_path, args.mid_batch,
                                               args.dispatch_gate)
        else:
            result = record_score_result(args.state, args.task_id, args.result, args.evidence,
                                         args.progress_file, args.audit_ns)
        print(json.dumps(result, ensure_ascii=False))
    elif args.command == "agent-events-watch":
        for line in watch_agent_events(args.events, args.cursor, args.acknowledgements,
                                       args.gate, args.handoff, replay_pending=args.replay_pending):
            print(line)
    elif args.command == "agent-event-ack":
        print(json.dumps({"event_id": args.event_id,
                          "recorded": acknowledge_agent_event(args.events,
                              args.acknowledgements, args.event_id, args.resolution)}))
    elif args.command == "ghost-prompt":
        atomic_write(args.output, build_ghost_closeout_prompt(args.task_id, args.verify, args.progress_file))
    elif args.command == "queue-pop":
        task = pop_task(args.path)
        if task:
            print(task)
    elif args.command == "queue-insert":
        print(json.dumps({"inserted": insert_task_front(args.tasks, args.task)}))
    elif args.command == "task-field":
        print(task_field(args.raw, args.key))
    elif args.command == "status-display":
        display_status(args.path)
    elif args.command == "task-cli":
        if args.ghost_state_file:
            args.ghost_state_path = args.ghost_state_file
        if args.score_state_file:
            args.agent_score_state_file = args.score_state_file
        return execute_adapter_task(args)
    elif args.command == "attempt-snapshot":
        print(capture_attempt_baseline(args.work_dir, args.task_id, args.attempt_id,
                                       args.snapshot))
    elif args.command == "attempt-recover":
        print(recover_attempt_baseline(args.work_dir, args.snapshot, args.archive))
    elif args.command == "adapter-exec":
        request_path = Path(args.request)
        request = json.loads(request_path.read_text(encoding="utf-8")) if request_path.exists() else {
            "registration_id": "contract-test", "overrides": {},
            "output_dir": str(request_path.parent / "adapter-output"),
        }
        status = execute_adapter_protocol(args.adapter_command, request, args.adapter_arg, args.trace,
                                          callback_timeout=args.callback_timeout,
                                          execution_timeout=args.execution_timeout)
        print(json.dumps(status, ensure_ascii=False))
    elif args.command == "run-timeout":
        command = args.argv[1:] if args.argv[:1] == ["--"] else args.argv
        return run_with_timeout(args.seconds, command)
    elif args.command == "scratch-review-check":
        project = Path(args.root).resolve()
        runtime = project / "nightshift" if (project / "nightshift").is_dir() else project
        result = (scratch_review.inventory(runtime) if args.stats_only else
                  scratch_review.check(runtime, runtime / "tasks.txt", insert_task_front, atomic_write))
        print(json.dumps(result))
    elif args.command == "code-index":
        result = run_code_index(Path.cwd(), args.indexer_args)
        return result.returncode
    elif args.command == "git-audit":
        code, reason = git_audit(args)
        if reason:
            print(reason)
        return code
    elif args.command == "archive-reports":
        archive_active_reports(args.root)
    elif args.command == "refresh-reports":
        refresh_active_reports(args.root, args.work_dir)
    elif args.command == "archive-overlapping-reports":
        archive_overlapping_reports(args.root, args.work_dir, args.base)
    elif args.command == "rollback-failed-task":
        paths = [path for path in (args.tasks, args.tasks_done, args.progress) if path]
        print(rollback_failed_task(args.root, args.work_dir, args.task_id, paths))
    elif args.command == "progress-prepend":
        prepend_progress(args.path, args.entry)
    elif args.command == "mechanical-audit":
        result = mechanical_audit(args)
        for message in result["messages"]:
            print(message, file=sys.stderr)
        print(json.dumps(result, ensure_ascii=False))
    elif args.command == "poll-next":
        sys.stdout.buffer.write(("\0".join(poll_next(args)) + "\0").encode("utf-8"))
    elif args.command == "daemon":
        return worker_daemon(args.root)
    else:
        write_status(args)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, TimeoutError, ValueError, RuntimeError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        print(f"worker_runtime: {error}", file=sys.stderr)
        sys.exit(2)
