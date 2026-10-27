#!/usr/bin/env python3
"""Codex executable adapter for NightShift's version 1 JSON protocol."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def response(value):
    print(json.dumps(value, ensure_ascii=False))


def read_request():
    value = json.load(sys.stdin)
    if value.get("protocol_version") != 1:
        raise ValueError("protocol_version 1 is required")
    return value


def config(request):
    overrides = request.get("overrides", {})
    approval_mode = overrides.get("approval_mode", "auto")
    if approval_mode not in ("auto", "approve-for-me", "prompt", "default", "", "bypass"):
        raise ValueError(f"unsupported Codex approval mode: {approval_mode}")
    return {"protocol_version": 1, "initial_score": int(overrides.get("initial_score", 5)),
            "limits": {"poll_interval_seconds": 0.25,
                       "callback_timeout_seconds": 30},
            "configuration": {"model": overrides.get("model", ""),
                              "effort": overrides.get("effort", "medium"),
                              "approval_mode": approval_mode,
                              "sandbox": overrides.get("sandbox", "workspace-write"),
                              "executable": overrides.get("command", overrides.get("executable", "codex")),
                              "agent_args": overrides.get("args", []),
                              "ephemeral": str(overrides.get("ephemeral", "1")),
                              "additional_dirs": overrides.get("additional_dirs", ""),
                              "extra_args": overrides.get("extra_args", [])}}


def start(request):
    cfg = request["configuration"]
    output = Path(request["output_dir"])
    output.mkdir(parents=True, exist_ok=True)
    status_path = output / "status.json"
    result_path = Path(request["result_path"])
    log_path = output / "codex.jsonl"
    message_path = output / "codex-message.json"
    prompt = Path(request["prompt_path"]).read_text(encoding="utf-8")
    argv = [cfg["executable"], "exec", "--color", "never", "--json",
            "--output-last-message", str(message_path)]
    if str(cfg.get("ephemeral", "1")).lower() in ("1", "true"):
        argv.append("--ephemeral")
    if cfg["model"]:
        argv += ["--model", cfg["model"]]
    if cfg["effort"]:
        argv += ["--config", f"model_reasoning_effort={cfg['effort']}"]
    if cfg["approval_mode"] in ("auto", "approve-for-me"):
        argv.append("--approve-for-me")
    elif cfg["approval_mode"] == "bypass":
        argv.append("--dangerously-bypass-approvals-and-sandbox")
    else:
        argv += ["--sandbox", cfg["sandbox"]]
    for directory in str(cfg.get("additional_dirs", "")).split(","):
        directory = directory.strip()
        if directory and Path(directory).is_dir():
            argv += ["--add-dir", directory]
    extra_args = cfg.get("extra_args", [])
    if not isinstance(extra_args, list) or not all(isinstance(arg, str) for arg in extra_args):
        raise ValueError("Codex extra_args must be a string array")
    argv.extend(extra_args)
    agent_args = cfg.get("agent_args", [])
    if not isinstance(agent_args, list) or not all(isinstance(arg, str) for arg in agent_args):
        raise ValueError("Codex agent args must be a string array")
    argv.extend(agent_args)
    limit = int(request.get("limits", {}).get("action_limit", 12))
    runner = r'''import json, os, re, signal, subprocess, sys
argv, cwd, env, log, message, result, status, limit = sys.argv[1:]
argv, env = json.loads(argv), json.loads(env)
with open(log, "w", encoding="utf-8") as stream:
    process = subprocess.Popen(argv, cwd=cwd, env={**os.environ, **env},
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1)
    process.stdin.write(open(sys.stdin.fileno()).read()); process.stdin.close()
    actions, over_budget, error_messages = 0, False, []
    for line in process.stdout:
        stream.write(line); stream.flush()
        try:
            event = json.loads(line); item = event.get("item", {})
            actions += event.get("type") == "item.completed" and item.get("type") == "command_execution"
            if event.get("type") in ("error", "turn.failed"):
                error = event.get("error")
                error_message = event.get("message") or (error.get("message") if isinstance(error, dict) else error)
                if isinstance(error_message, str): error_messages.append(error_message)
        except (ValueError, AttributeError):
            pass
        if actions > int(limit) and process.poll() is None:
            over_budget = True; process.terminate()
    try: code = process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        process.kill(); code = process.wait(); over_budget = True
if actions > int(limit): over_budget = True
diagnostic = "\n".join(error_messages)
if over_budget:
    outcome = "step_budget_exceeded"
elif code != 0 and actions == 0:
    outcome = "no_response"
elif code == 0:
    outcome = "complete"
else:
    outcome = "failure"
text = ""
try:
    with open(message, encoding="utf-8") as source: text = source.read()
except OSError: pass
try: payload = json.loads(text)
except ValueError:
    payload = None
    try:
        clean = text.lstrip()
        candidate, end = json.JSONDecoder().raw_decode(clean)
        legacy = re.fullmatch(r"\s*Attempts:\s*(\d+)\s*\|\s*Confidence:\s*(HIGH|MED|LOW)\s*",
                              clean[end:])
        if isinstance(candidate, dict) and legacy:
            payload = {**candidate, "attempts": int(legacy.group(1)),
                       "confidence": legacy.group(2)}
    except ValueError: pass
if not isinstance(payload, dict):
    markers = (("SCOPE_BLOCKED", "scope_blocked"), ("BLOCKED", "blocked"),
               ("NEED_GUIDANCE", "guidance"), ("NEED_DECOMPOSITION", "guidance"),
               ("CLOSEOUT_INCOMPLETE", "blocked"), ("CLOSEOUT_REQUIRED", "closeout_required"))
    final_outcome = next((value for marker, value in markers
                          if re.search(r"\[" + marker + r"\]", text)), "failure")
    payload = {"outcome": final_outcome, "diagnostic": text or diagnostic or "no response"}
    if final_outcome == "failure":
        if code != 0 and actions == 0:
            outcome = "no_response"
            payload["outcome"] = "no_response"
            payload["error_code"] = "no_response"
            payload["diagnostic"] = text or diagnostic or "no response"
        else:
            payload["error_code"] = "invalid_result"
allowed = {"complete", "no_change", "closeout_required", "blocked", "scope_blocked", "guidance", "failure", "no_response"}
if not isinstance(payload.get("outcome"), str) or payload["outcome"] not in allowed:
    payload = {"outcome": "failure", "error_code": "invalid_result",
               "diagnostic": "Codex final JSON requires a supported outcome"}
payload["actions_used"] = actions
payload["cli_exit_code"] = code
if outcome != "complete":
    payload["outcome"] = outcome
    final_status = "failed" if outcome == "failure" else outcome
else:
    # CLI success only completes execution; the final result decides task success.
    final_status = "complete" if payload["outcome"] in ("complete", "no_change") else (
        "closeout_required" if payload["outcome"] == "closeout_required" else "failed")
json.dump(payload, open(result, "w", encoding="utf-8"))
json.dump({"status": final_status, "result_location": result, "exit_code": code}, open(status, "w", encoding="utf-8"))
'''
    child_env = {**request.get("environment", {}),
                 "NIGHTSHIFT_TASK_ID": request.get("task_id", ""),
                 "NIGHTSHIFT_EXECUTION_KIND": request.get("kind", "main"),
                 "PORT": request.get("environment", {}).get("PORT", "3001"),
                 "TEST_PORT": request.get("environment", {}).get("TEST_PORT", "3001")}
    proc = subprocess.Popen([sys.executable, "-c", runner, json.dumps(argv),
                             request["working_directory"], json.dumps(child_env),
                             str(log_path), str(message_path), str(result_path),
                             str(status_path), str(limit)], stdin=subprocess.PIPE,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=(os.name == "posix"))
    proc.stdin.write(prompt.encode("utf-8")); proc.stdin.close()
    response({"execution_id": str(proc.pid), "status_location": str(status_path)})


def status(request):
    path = Path(request["status_location"])
    if path.is_file():
        response(json.loads(path.read_text(encoding="utf-8")))
    else:
        response({"status": "running"})


def cancel(request):
    pid = int(request["execution_id"])
    try:
        os.killpg(pid, signal.SIGTERM) if os.name == "posix" else os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        response({"stopped": True}); return
    deadline = time.monotonic() + float(request.get("stop_timeout_seconds", 2))
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
            stat = Path(f"/proc/{pid}/stat")
            if stat.exists() and stat.read_text().split()[2] == "Z":
                response({"stopped": True}); return
        except ProcessLookupError:
            response({"stopped": True}); return
        time.sleep(0.05)
    response({"stopped": False})


def closeout(request):
    request["kind"] = "closeout"
    request.setdefault("limits", {})["action_limit"] = request["limits"].get("action_limit", 1)
    # Closeout uses a distinct, private Codex execution with the same local status contract.
    start(request)


def main():
    callback = sys.argv[-1]
    request = read_request()
    {"get_config": lambda: response(config(request)), "start": lambda: start(request),
     "status": lambda: status(request), "cancel": lambda: cancel(request),
     "closeout": lambda: closeout(request)}[callback]()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"codex adapter: {error}", file=sys.stderr)
        sys.exit(1)
