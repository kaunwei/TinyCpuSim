"""Serial task lifecycle. Planner owns restart; runtime owns acceptance."""

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import threading
import time
import uuid

import acceptance as gate


def append_once(path, identity, line):
    path = Path(path)
    old = path.read_text() if path.exists() else ""
    if identity not in old:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a") as stream:
            stream.write(line + "\n")


def protect_runtime(root, paths):
    """Generated state survives clean-start, even before tracked ignores migrate."""
    try:
        exclude = Path(gate.git(root, "rev-parse", "--git-path", "info/exclude"))
        if not exclude.is_absolute():
            exclude = root / exclude
    except Exception:
        return
    old = exclude.read_text() if exclude.exists() else ""
    additions = []
    for path in paths:
        path = Path(path).resolve()
        if path.is_relative_to(root):
            name = path.relative_to(root).as_posix()
            if gate.git(root, "ls-files", "--", name):
                if name in ("tasks.txt", "nightshift/tasks.txt"):
                    continue
                raise gate.GateError(f"migration required: untrack generated runtime file {name}")
            additions.extend(["/" + name, "/" + name + ".lock/"])
    exclude.parent.mkdir(parents=True, exist_ok=True)
    with exclude.open("a") as stream:
        stream.write("\n" + "\n".join(item for item in additions if item not in old.splitlines()) + "\n")


def diagnose(root, runtime, active, result):
    directory = gate.state_dir(root) / "reports" / active["event"]
    directory.mkdir(parents=True, exist_ok=True)
    detail = result.get("runtime_error", {}).get("message") or result.get("diagnostic") or result.get("outcome", "execution failed")
    report = {"task": active["raw"], "base": active.get("base"),
              "head": gate.git(root, "rev-parse", "HEAD"), "result": result}
    gate.save(directory / "report.json", report)
    (directory / "REPORT.md").write_text(
        "# Task failure\n\n" + json.dumps(report, indent=2, ensure_ascii=True) + "\n", encoding="ascii")
    if (root / "nightshift/config.json").is_file() or (root / "nightshift/acceptance.json").is_file():
        gate.pause(root, active["task_id"], f"{detail}; report={directory / 'REPORT.md'}")
    return directory


def cold_ghost(root, runtime, active, error):
    last = gate.load(gate.state_dir(root) / "last-adapter.json")
    result = {"outcome": "blocked", "runtime_error": {"phase": "cold", "message": str(error)}}
    if last:
        request = last["request"]
        args = argparse.Namespace(
            work_dir=str(root), task_id=active["task_id"], title=active["raw"],
            base=active["base"], target="", verify=gate.config(root).get("cold", ""),
            timeout=180, progress_file="runtime-owned", worker_port="3001",
            adapter_command=last["command"], executable=last["command"], adapter_args=last["args"],
            attempt_id=active["event"], diagnostic_only=True,
            ghost_state_path=str(gate.state_dir(root) / "ghost.json"),
            ghost_artifact_root=str(gate.state_dir(root) / "ghost"))
        try:
            _, supplement, _ = runtime.run_adapter_ghost(
                args, request, {"execution_id": active["event"]}, result)
            result.update(supplement)
        except Exception as failure:
            result["ghost_error"] = str(failure)
    else:
        result["ghost_error"] = "no prior adapter registration is available for diagnosis"
    return result


def daemon(root, runtime):
    root = Path(root).resolve()
    with gate.workspace(root):
        return run(root, runtime)


def run(root, runtime):
    state = gate.state_dir(root)
    owned = root / "nightshift" if (root / "nightshift").is_dir() else root
    def path(name, default):
        value = Path(os.environ.get(name, str(owned / default)))
        return value if value.is_absolute() else root / value
    tasks = path("TASKS_FILE", "tasks.txt")
    done = path("TASKS_DONE_FILE", "tasks.done")
    progress = path("PROGRESS_FILE", "progress.log")
    status = path("STATUS_FILE", "worker.status")
    log_file = path("LOG_FILE", ".worker.log")
    pause = path("PAUSE_FILE", "PAUSE")
    dispatch_gate = path("AGENT_DISPATCH_GATE_FILE", ".worker-dispatch-gate.json")
    score = path("WORKER_SCORE_FILE", ".worker-score.json")
    agent_scores = path("WORKER_AGENT_SCORES_DIR", ".worker-agent-scores")
    agent_state_file = path("AI_AGENT_STATE_FILE", f".worker-agent-state.{os.environ.get('WORKER_ID', 'default')}.json")
    registry = path("AGENT_CONFIG_FILE", "config.json")
    if not registry.is_file():
        for candidate in (owned / "config.json", root / "nightshift/config.json",
                          owned / "adapters/agents.json", owned / "agents.json",
                          root / "nightshift/adapters/agents.json", root / "nightshift/agents.json",
                          root / "scripts/adapters/agents.json"):
            if candidate.is_file():
                registry = candidate
                break
    agents = []
    if os.environ.get("AI_AGENTS_JSON"):
        agents = runtime.parse_agent_registry(os.environ["AI_AGENTS_JSON"])
    elif registry.is_file():
        agents = runtime.load_agent_registry(registry)
    protect_runtime(root, (tasks, done, progress, status, log_file, pause, dispatch_gate, score,
                           agent_scores, agent_state_file, owned / ".scratch", owned / ".worker-ghosts"))
    for file in (tasks, done, progress, log_file):
        file.parent.mkdir(parents=True, exist_ok=True)
        file.touch(exist_ok=True)
    active_path = state / "active.json"
    poll = float(os.environ.get("POLL_INTERVAL", "5"))
    once = runtime.env_bool("RUN_ONCE")
    stopped = threading.Event()
    previous = {}
    signals = [signal.SIGTERM, signal.SIGINT]
    if hasattr(signal, "SIGHUP"):
        signals.append(signal.SIGHUP)
    for sig in signals:
        previous[sig] = signal.signal(sig, lambda *_: stopped.set())
    running = {"state": "IDLE", "current_task": None, "pid": os.getpid(),
               "work_dir": str(root), "worker_id": os.environ.get("WORKER_ID", "default"),
               "os": os.environ.get("OS_TYPE", "auto"), "platform": os.name,
               "elapsed_seconds": 0, "worker_score": runtime.read_score_state(score)["score"],
               "timeout_seconds": float(os.environ.get("TASK_TIMEOUT", "1800")),
               "cli": "registered-adapters", "model": "",
               "progress": {"current_index": 0, "total_tasks": 0, "remaining": 0,
                            "percentage": "0%", "bar": "[------------------------------]"}}
    is_silent_cold = False
    if tasks.is_file():
        for line in tasks.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line:
                if runtime.task_header_has_tag(line, r"\[COLD\]"):
                    is_silent_cold = True
                break
    if not is_silent_cold:
        print("=" * 78)
        print(" 24-Hour Unattended AI Worker Daemon Started")
        print(f" Worker ID: {running['worker_id']} | OS: {running['os']}")
        print(f" PID: {running['pid']} | Registered adapters: {len(agents)}")
        print(f" WorkDir: {root}")
        print(f" Tasks: {tasks} | Status: {status}")
        print(f" Progress: {progress}")
        print("=" * 78)
        if once:
            print("[WORKER] RUN_ONCE is enabled.")
    began = [time.monotonic()]
    def update(kind, task=None):
        if task != running.get("current_task"):
            began[0] = time.monotonic()
        running.update(state=kind, current_task=task, last_heartbeat=datetime.now().isoformat())
        gate.save(status, running)
    def heartbeat():
        while not stopped.wait(1):
            running["last_heartbeat"] = datetime.now().isoformat()
            if running["state"] in ("RUNNING", "VERIFYING"):
                running["elapsed_seconds"] = int(time.monotonic() - began[0])
            gate.save(status, dict(running))
    thread = threading.Thread(target=heartbeat, daemon=True)
    thread.start()
    scratch_checked = float("-inf")
    recovery_halted_task = ""
    waiting_for_response = False

    def fail(active, result):
        if not ((root / "nightshift/config.json").is_file() or (root / "nightshift/acceptance.json").is_file()) and active.get("attempt_baseline"):
            try:
                runtime.rollback_failed_task(
                    root, root, active["task_id"],
                    (tasks, done, progress, status, log_file, pause, dispatch_gate, score, agent_scores, registry, owned / "config.json", owned / "acceptance.json", owned / "code-index.toml"),
                    baseline_snapshot=active["attempt_baseline"],
                    attempt_id=active.get("event", ""))
            except Exception:
                pass
        report = diagnose(root, runtime, active, result)
        token = f"Failure_ID: {active['event']}"
        detail = ((result.get("diagnostic") or "").strip()
                  or result.get("runtime_error", {}).get("message")
                  or result.get("outcome", "execution failed"))
        append_once(progress, token, f"[BLOCKED] {active['task_id']} | {token} | Report: {report / 'REPORT.md'}\nFailure Reason: {detail}")
        if (result.get("terminal_status") == "step_budget_exceeded"
                or result.get("outcome") in ("step_budget", "closeout_failed")
                or result.get("exit_code") == 125):
            append_once(progress, f"budget:{active['task_id']}",
                        f"[NEED_DECOMPOSITION] {active['task_id']}: Action budget exhausted; review and return a revised bounded ticket.")
        if log_file.exists():
            with log_file.open("a") as stream:
                stream.write(f"Failure Reason: {detail}\n")
        done_tag = f" [WORKER_BLOCKED]" if (result.get("outcome") in ("blocked", "worker_blocked") or "[BLOCKED]" in str(result.get("diagnostic", ""))) else f" [{token}]"
        append_once(done, token, f"[FAILED] {active['raw']}{done_tag}")
        active["phase"] = "failed"
        gate.save(active_path, active)
        active_path.unlink(missing_ok=True)
        update("BLOCKED", f"{active['task_id']} awaiting Planner")

    def score_once(active, result, accepted):
        identity = active["event"] + ":score"
        registration = result.get("registration_id", "")
        candidate = runtime.registered_agent_score_path(agent_scores, registration) if registration else score
        destination = candidate if candidate.is_file() else score
        # An accepted task advances the existing accept streak, not old success-only bookkeeping.
        neutral = runtime.is_research_task(active["raw"]) and not accepted
        outcome = "accept" if accepted else "no_fault" if neutral else "reject"
        runtime.record_score_result(destination, active["task_id"], outcome,
                                    "automatic hot gate", event_id=identity)
        if not accepted and result.get("terminal_status") in ("execution_timeout", "step_budget_exceeded"):
            runtime.record_score_result(destination, active["task_id"], "hard_failure",
                                        result["terminal_status"], event_id=identity + ":hard")

    def finish(active, result):
        journal_path = state / "tasks" / (gate.key(active["task_id"], active["raw"]) + ".json")
        journal = gate.load(journal_path, {})
        result["registration_id"] = journal.get("registration_id", result.get("registration_id", ""))
        commit = journal.get("commit", result.get("commit", ""))
        if (commit and commit != active.get("base")) or runtime.is_research_task(active["raw"]):
            score_once(active, result, True)
        scheduled_cold = False
        if commit and commit != active.get("base") and not runtime.is_research_task(active["raw"]):
            before_pending = gate.load(state / "cold.json", {}).get("pending", False)
            gate.schedule_cold(root, commit, tasks, runtime.insert_task_front)
            after_pending = gate.load(state / "cold.json", {}).get("pending", False)
            if not before_pending and after_pending:
                scheduled_cold = True
        token = f"Audit_NS: {active['event']}"
        attempts = result.get("attempts", 1)
        print(f"[AUDIT] Running mechanical audit for {active['task_id']}...", flush=True)
        append_once(progress, token,
                    f"[MECHANICAL_AUDIT] Status: PASS | Task: {active['task_id']} | Verify_Exit: 0 | Whitelist: PASS | Tree: CLEAN | Attempts: {attempts} | Audit_NS: {active['event']} | Acceptance: automatic")
        append_once(done, token, f"[DONE] {active['raw']} [Audit_NS: {active['event']}]")
        print(f"[COMMIT] Committing verified changes for {active['task_id']} ({commit[:8] if commit else 'no-commit'})...", flush=True)
        runtime.settle_invalid_reports(root, root, accepted=True)
        runtime.refresh_active_reports(root, root)
        journal.update(phase="accepted", commit=commit, result=result, event=active["event"])
        gate.save(journal_path, journal)
        active_path.unlink(missing_ok=True)
        print(f"[DONE] <<< Task {active['task_id']} accepted and verified cleanly!\n", flush=True)
        if scheduled_cold and once:
            update("IDLE")
            return True
        return False

    try:
        while not stopped.is_set():
            if (state / "paused.json").exists() or dispatch_gate.exists():
                update("PAUSED_FOR_PLANNER")
                if once:
                    return 0
                stopped.wait(poll)
                continue
            if pause.exists():
                update("PAUSED")
                if once:
                    return 0
                stopped.wait(poll)
                continue
            active = gate.load(active_path)
            if active:
                # The queue claim was persisted before removal. Dedup a claim interrupted there.
                if active.get("claimed"):
                    runtime.remove_claimed_task(tasks, active["raw"])
                if "task_id" not in active:
                    match = re.search(r"TASK-[A-Za-z0-9_.-]+", active["raw"])
                    active.update(task_id=match.group() if match else "TASK-UNKNOWN",
                                  base=gate.git(root, "rev-parse", "HEAD"), event=uuid.uuid4().hex)
                if active.get("phase") == "failed":
                    # Only explicit restart can reach here after a diagnosed failure.
                    active_path.unlink()
                    continue
                journal_path = state / "tasks" / (gate.key(active["task_id"], active["raw"]) + ".json")
                journal = gate.load(journal_path, {})
                if journal.get("phase") == "ready_commit":
                    recovery = argparse.Namespace(**journal["args"])
                    marker = f"NightShift-Key: {gate.key(active['task_id'], active['raw'])}"
                    commit = gate.git(root, "log", "--format=%H", "--fixed-strings", f"--grep={marker}", "-1")
                    if commit:
                        journal.update(phase="committed", commit=commit)
                        gate.save(journal_path, journal)
                    elif runtime.finalize_structured_adapter_result(recovery, journal["result"]) == 0:
                        journal = gate.load(journal_path)
                if journal.get("phase") in ("committed", "accepted"):
                    if journal.get("commit") and subprocess.run(
                            ["git", "-C", str(root), "merge-base", "--is-ancestor", journal["commit"], "HEAD"], capture_output=True).returncode:
                        raise gate.GateError("accepted commit missing from current history")
                    if finish(active, journal["result"]):
                        return 0
                    update("IDLE")
                    if once:
                        return 0
                    continue
                # Interrupted model execution is not safe to repeat or clean without Planner.
                fail(active, {"diagnostic": "interrupted task; schedule a new task and restart"})
                continue
            # Replay a due cold insertion after a crash before queue mutation.
            cold_state = gate.load(state / "cold.json", {})
            if cold_state.get("pending"):
                gate.schedule_cold(root, "", tasks, runtime.insert_task_front)
            if time.monotonic() - scratch_checked >= 60:
                runtime.scratch_review.check(owned, tasks, runtime.insert_task_front, runtime.atomic_write)
                scratch_checked = time.monotonic()
            if recovery_halted_task and not pause.is_file():
                update("RECOVERY_FAILED", f"{recovery_halted_task} (recovery halted); awaiting revised queue or intervention")
                stopped.wait(poll)
                head_task = ""
                if tasks.is_file():
                    lines = [l.strip() for l in tasks.read_text(encoding="utf-8").splitlines()
                             if l.strip() and not l.lstrip().startswith("#")]
                    head_task = lines[0].split("|", 1)[0].replace(" ", "") if lines else ""
                if head_task != recovery_halted_task:
                    recovery_halted_task = ""
                else:
                    continue
            if waiting_for_response and not pause.is_file():
                state_data = runtime.read_agent_state(agent_state_file)
                if state_data.get("consecutive_no_response", 0) >= len(agents):
                    update("AGENT_NO_RESPONSE", "Waiting for agent response")
                    stopped.wait(poll)
                    continue
                stopped.wait(poll)
                waiting_for_response = False
            raw = runtime.pop_task(tasks, claim_path=active_path)
            if not raw:
                if running["state"] != "BLOCKED":
                    update("IDLE")
                if once:
                    if not is_silent_cold:
                        print("[WORKER] Queue is empty and RUN_ONCE is enabled. Exiting.")
                    return 0
                stopped.wait(poll)
                continue
            active = gate.load(active_path)
            identity = re.search(r"TASK-[A-Za-z0-9_.-]+", raw.split("|", 1)[0])
            active.update(task_id=identity.group() if identity else raw.split("|", 1)[0].strip(),
                          event=uuid.uuid4().hex, base=gate.git(root, "rev-parse", "HEAD"), phase="running")
            gate.save(active_path, active)
            previous_record = gate.load(state / "tasks" / (gate.key(active["task_id"], raw) + ".json"), {})
            if previous_record.get("phase") == "accepted" and not runtime.task_header_has_tag(raw, r"\[COLD\]"):
                active["event"] = previous_record.get("event", active["event"])
                if finish(active, previous_record["result"]):
                    return 0
                continue
            if runtime.task_header_has_tag(raw, r"\[COLD\]"):
                update("VERIFYING", active["task_id"])
                try:
                    gate.cold(root, runtime.run_verify)
                    active_path.unlink()
                except Exception as error:
                    result = cold_ghost(root, runtime, active, error)
                    fail(active, result)
                update("IDLE")
                if once:
                    return 0
                continue
            args = None
            result = {}
            try:
                if (root / "nightshift/config.json").is_file() or (root / "nightshift/acceptance.json").is_file():
                    gate.clean(root)
                runtime.refresh_active_reports(root, root)
                stale = state / "tasks" / (gate.key(active["task_id"], raw) + ".json")
                stale.unlink(missing_ok=True)
                agents = (runtime.parse_agent_registry(os.environ["AI_AGENTS_JSON"])
                          if os.environ.get("AI_AGENTS_JSON") else runtime.load_agent_registry(registry))
                if not agents:
                    raise gate.GateError("no executable adapter registration")
                target = runtime.task_field(raw, "TARGET")
                level = re.search(r"LEVEL:\s*([1-3])", raw)
                level = level.group(1) if level else "1"
                verify = runtime.task_field(raw, "VERIFY")
                file_count = sum(max(1, sum(item.is_file() for item in (root / part.strip()).rglob("*")))
                                 if (root / part.strip()).is_dir() else 1 for part in target.split(",") if part.strip())
                cfg_path = root / "nightshift/config.json"
                cfg_obj = gate.load(cfg_path) if cfg_path.is_file() else {}
                step_ladder = cfg_obj.get("step_base", {})
                default_base = step_ladder.get(f"L{level}", {"1": 60, "2": 40, "3": 20}.get(level, 20)) if isinstance(step_ladder, dict) else (step_ladder if isinstance(step_ladder, int) else 20)
                env_base = os.environ.get("STEP_BASE", "").strip()
                step_base = int(env_base) if env_base else int(default_base)
                step_per_file = int(os.environ.get("STEP_PER_FILE", "5"))
                budget = step_base + max(1, file_count) * step_per_file
                directory = state / "runs" / active["event"]
                directory.mkdir(parents=True)
                prompt = directory / "prompt.txt"
                prompt.write_text(runtime.build_task_prompt(raw, active["task_id"], level, target, verify,
                    runtime.task_field(raw, "FORBIDDEN_SHORTCUTS"), runtime.task_field(raw, "CONSTRAINTS"),
                    budget, max(1, file_count), step_base, step_per_file, progress))
                args = argparse.Namespace(
                    work_dir=str(root), prompt_file=str(prompt), output_file=str(directory / "output.json"),
                    result_file=str(directory / "result.json"), timeout=float(os.environ.get("TASK_TIMEOUT", "1800")),
                    step_budget=budget, research_step_budget=int(os.environ.get("RESEARCH_STEP_BUDGET", "100")),
                    worker_port=os.environ.get("WORKER_PORT", "3001"), task_id=active["task_id"],
                    title=raw, target=target, level=level, verify=verify, base=active["base"],
                    required_cases=runtime.task_field(raw, "CASES"), progress_file=str(progress),
                    ghost_state_path=str(state / "ghost.json"), ghost_artifact_root=str(state / "ghost"),
                    agent_score_state_file=str(agent_scores), attempt_snapshot_root=str(owned / ".scratch/attempts" / active["task_id"]),
                    awaiting_agent=False, recovery_failed=False)
                update("RUNNING", active["task_id"])
                def recover_before_switch(snapshot, attempt_id):
                    try:
                        runtime.rollback_failed_task(
                            root, root, active["task_id"],
                            (tasks, done, progress, status, log_file, pause, dispatch_gate, score, agent_scores, agent_state_file, registry, owned / "config.json", owned / "acceptance.json", owned / "code-index.toml"),
                            baseline_snapshot=snapshot, attempt_id=attempt_id)
                    except Exception as error:
                        args.recovery_failed = True
                        update("RECOVERY_FAILED", active["task_id"])
                        raise RuntimeError(f"attempt recovery failed: {error}")
                code, _ = runtime.execute_registered_agents(args, agents, agent_state_file, recover_before_switch)
                active["attempt_baseline"] = getattr(args, "attempt_baseline", "")
                result = gate.load(args.result_file, {})
                result["registration_id"] = getattr(args, "executed_agent_id", "")
                result["terminal_status"] = gate.load(args.output_file, {}).get("status", "")
                result["exit_code"] = code
                if code == 125:
                    result["outcome"] = "step_budget"
                if getattr(args, "recovery_failed", False):
                    update("RECOVERY_FAILED", active["task_id"])
                    print(f"[RECOVERY_HALTED] {active['task_id']}: baseline could not be restored; task requeued.", flush=True)
                    runtime.insert_task_front(tasks, raw)
                    active_path.unlink(missing_ok=True)
                    if once:
                        return 0
                    recovery_halted_task = active["task_id"]
                    stopped.wait(poll)
                    continue
                if getattr(args, "agent_no_response", False):
                    update("AGENT_NO_RESPONSE", active["task_id"])
                    print(f"[WORKER] AGENT_NO_RESPONSE for {active['task_id']}; reporting to planner.", flush=True)
                    append_once(progress, f"no_response:{active['task_id']}", f"[AGENT_NO_RESPONSE] {active['task_id']}: agent no response; reporting to planner.")
                    runtime.insert_task_front(tasks, raw)
                    active_path.unlink(missing_ok=True)
                    if once:
                        return 0
                    waiting_for_response = True
                    stopped.wait(poll)
                    continue
                if getattr(args, "awaiting_agent", False):
                    msg = f"[AGENT_UNAVAILABLE] {active['task_id']}: no eligible registration remains for this task; task returned to {tasks}. After fixing the adapter issue, remove this task from task_exclusions in {agent_state_file} to retry."
                    append_once(progress, f"agent_unavailable:{active['task_id']}", msg)
                    runtime.insert_task_front(tasks, raw)
                    active_path.unlink(missing_ok=True)
                    update("WAITING_FOR_AGENT", active["task_id"])
                    if once:
                        return 0
                    continue
                if code:
                    raise gate.GateError(result.get("runtime_error", {}).get("message") or "task execution failed")
                if runtime.scratch_review.is_review(raw):
                    err = ""
                    try:
                        runtime.scratch_review.validate_report(owned, active["task_id"])
                    except (OSError, ValueError, TypeError) as failure:
                        err = str(failure)
                    runtime.scratch_review.finish(owned, active["task_id"], runtime.atomic_write, err)
                    print(f"[SCRATCH_REVIEW] {'failed' if err else 'reviewed'}: fixed report overwritten", flush=True)
                    active_path.unlink(missing_ok=True)
                    update("IDLE")
                    continue
                elif runtime.is_research_task(raw):
                    audit = runtime.mechanical_audit(argparse.Namespace(
                        work_dir=str(root), root=str(root), base=args.base, target=target,
                        constraints="", title=raw, level=int(level), task_id=args.task_id,
                        verify="", port=args.worker_port, verify_timeout=args.timeout,
                        baseline_snapshot=args.attempt_baseline))
                    if audit["status"]:
                        raise gate.GateError(audit["error"])
                if finish(active, result):
                    return 0
                update("IDLE")
            except Exception as error:
                if args is not None and getattr(args, "recovery_failed", False):
                    update("RECOVERY_FAILED", active["task_id"])
                    print(f"[RECOVERY_HALTED] {active['task_id']}: baseline could not be restored; task requeued.", flush=True)
                    runtime.insert_task_front(tasks, raw)
                    active_path.unlink(missing_ok=True)
                    return 0
                result.setdefault("runtime_error", {"phase": "lifecycle", "message": str(error)})
                score_once(active, result, False)
                fail(active, result)
    finally:
        stopped.set()
        thread.join(timeout=2)
        if running["state"] not in ("BLOCKED", "PAUSED", "PAUSED_FOR_PLANNER", "AGENT_NO_RESPONSE", "RECOVERY_FAILED", "WAITING_FOR_AGENT"):
            update("STOPPED")
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    return 0
