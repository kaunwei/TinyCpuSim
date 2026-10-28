#!/usr/bin/env python3
"""AgY executable adapter for NightShift's version 1 JSON protocol."""
import json
import os
from pathlib import Path
import shutil
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


def resolve_executable(executable):
    if os.path.isabs(executable) or os.path.dirname(executable):
        return executable
    resolved = shutil.which(executable)
    if resolved:
        return resolved
    for candidate_dir in (Path.home() / ".local/bin", Path("/opt/homebrew/bin"), Path("/usr/local/bin")):
        candidate = candidate_dir / executable
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return executable


def config(request):
    overrides = request.get("overrides", {})
    return {"protocol_version": 1, "initial_score": int(overrides.get("initial_score", 5)),
            "limits": {"poll_interval_seconds": 0.25,
                       "callback_timeout_seconds": 30},
            "configuration": {"model": overrides.get("model", ""),
                              "effort": overrides.get("effort", "medium"),
                              "mode": overrides.get("mode", "accept-edits"),
                              "sandbox": overrides.get("sandbox", "0"),
                              "executable": overrides.get("command", overrides.get("executable", "agy")),
                              "agent_args": overrides.get("args", []),
                              "additional_dirs": overrides.get("additional_dirs", ""),
                              "extra_args": overrides.get("extra_args", []),
                              "execution_timeout_seconds": overrides.get("execution_timeout_seconds", 1800),
                              "action_limit": overrides.get("action_limit", 12)}}


def start(request):
    cfg = request["configuration"]
    output = Path(request["output_dir"])
    output.mkdir(parents=True, exist_ok=True)
    status_path = output / "status.json"
    result_path = Path(request["result_path"])
    log_path = output / "agy.log"
    message_path = output / "agy-message.json"
    prompt = Path(request["prompt_path"]).read_text(encoding="utf-8")
    executable = resolve_executable(cfg["executable"])
    argv = [executable, "-p", prompt, "--dangerously-skip-permissions"]
    if str(cfg.get("sandbox", "0")).lower() in ("1", "true"):
        argv.append("--sandbox")
    if cfg.get("mode"):
        argv += ["--mode", cfg["mode"]]
    if cfg.get("model"):
        argv += ["--model", cfg["model"]]
    if cfg.get("effort"):
        argv += ["--effort", cfg["effort"]]
    timeout = request.get("limits", {}).get("execution_timeout_seconds", cfg.get("execution_timeout_seconds", 1800))
    argv += ["--print-timeout", f"{timeout}s"]
    for directory in str(cfg.get("additional_dirs", "")).split(","):
        directory = directory.strip()
        if directory and Path(directory).is_dir():
            argv += ["--add-dir", directory]
    extra_args = cfg.get("extra_args", [])
    if not isinstance(extra_args, list) or not all(isinstance(arg, str) for arg in extra_args):
        raise ValueError("AgY extra_args must be a string array")
    argv.extend(extra_args)
    agent_args = cfg.get("agent_args", [])
    if not isinstance(agent_args, list) or not all(isinstance(arg, str) for arg in agent_args):
        raise ValueError("AgY agent args must be a string array")
    argv.extend(agent_args)
    limit = int(request.get("limits", {}).get("action_limit", cfg.get("action_limit", 12)))
    try:
        help_text = subprocess.run([executable, "--help"], cwd=request["working_directory"],
                                   capture_output=True, text=True, timeout=10).stdout
        if "--max-turns" in help_text:
            argv += ["--max-turns", str(limit)]
        elif "--max-steps" in help_text:
            argv += ["--max-steps", str(limit)]
    except (OSError, subprocess.TimeoutExpired):
        pass
    runner = r'''import json, os, re, signal, subprocess, sys
argv, cwd, env, log, message, result, status, limit = sys.argv[1:]
argv, env = json.loads(argv), json.loads(env)
with open(log, "w", encoding="utf-8") as stream:
    process = subprocess.Popen(argv, cwd=cwd, env={**os.environ, **env},
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1)
    try:
        input_data = open(sys.stdin.fileno(), encoding="utf-8").read()
        if input_data:
            process.stdin.write(input_data)
        process.stdin.close()
    except (BrokenPipeError, OSError):
        pass
    actions, over_budget, error_messages = 0, False, []
    for line in process.stdout:
        stream.write(line)
        stream.flush()
        try:
            event = json.loads(line)
            item = event.get("item", {})
            actions += event.get("type") == "item.completed" and item.get("type") == "command_execution"
            if event.get("type") in ("error", "turn.failed"):
                error = event.get("error")
                message_text = event.get("message") or (error.get("message") if isinstance(error, dict) else error)
                if isinstance(message_text, str):
                    error_messages.append(message_text)
        except (ValueError, AttributeError):
            pass
        if line.strip().startswith("[EXEC]"):
            actions += 1
        if actions > int(limit) and process.poll() is None:
            over_budget = True
            process.terminate()
    try:
        code = process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        code = process.wait()
        over_budget = True
if actions > int(limit):
    over_budget = True
full_log = open(log, encoding="utf-8", errors="replace").read()
diagnostic = "\n".join(error_messages)
timed_out = "print timeout after" in full_log.lower()
if timed_out:
    outcome = "execution_timeout"
elif over_budget:
    outcome = "step_budget_exceeded"
elif code != 0 and actions == 0:
    outcome = "no_response"
elif code == 0:
    outcome = "complete"
else:
    outcome = "failure"

text = ""
message_present = False
try:
    with open(message, encoding="utf-8") as source:
        text = source.read().strip()
        message_present = True
except OSError:
    pass

payload = None
invalid_payload = {"outcome": "failure", "error_code": "invalid_result",
                   "diagnostic": "AgY terminal result is malformed or absent"}
if message_present:
    payload = dict(invalid_payload)
    try:
        parsed = json.loads(text)
        if isinstance(parsed, dict):
            payload = parsed
    except ValueError:
        pass

if payload is None:
    candidates = []
    for m in re.finditer(r"```(?:[a-zA-Z0-9_-]+)?\s*([\s\S]*?)\s*```", full_log):
        block_text = m.group(1).strip()
        if block_text.startswith("{") and block_text.endswith("}"):
            try:
                parsed = json.loads(block_text)
                if isinstance(parsed, dict) and "outcome" in parsed:
                    candidates.append((m.end(), parsed))
            except ValueError:
                pass
    decoder = json.JSONDecoder()
    pos = 0
    while True:
        pos = full_log.find("{", pos)
        if pos == -1:
            break
        try:
            parsed, end_idx = decoder.raw_decode(full_log, pos)
            if isinstance(parsed, dict) and "outcome" in parsed:
                line_start = full_log.rfind("\n", 0, pos) + 1
                prefix = full_log[line_start:pos].strip()
                line_end = full_log.find("\n", end_idx)
                if line_end == -1:
                    line_end = len(full_log)
                suffix = full_log[end_idx:line_end].strip()
                if prefix in ("", "```") or prefix.startswith("```"):
                    if suffix in ("", "```") or suffix.endswith("```") or not full_log[end_idx:].strip():
                        candidates.append((end_idx, parsed))
            pos = end_idx
        except ValueError:
            line_start = full_log.rfind("\n", 0, pos) + 1
            prefix = full_log[line_start:pos]
            if not prefix.strip():
                # Do not reinterpret a nested object as an independent closeout
                # when its enclosing top-level result failed to decode.
                indent = len(prefix)
                next_pos = len(full_log)
                for following in re.finditer(r"(?m)^([ \t]*)(?=\{|```)", full_log[pos + 1:]):
                    if len(following.group(1)) <= indent:
                        next_pos = pos + 1 + following.start() + len(following.group(1))
                        break
                candidates.append((next_pos - 1, dict(invalid_payload)))
                pos = next_pos
            else:
                pos += 1
    if candidates:
        candidates.sort(key=lambda c: c[0])
        terminal_end, payload = candidates[-1]
        # A later incomplete structured closeout cannot revive historical success.
        suffix = full_log[terminal_end:]
        if re.search(r"(?m)^\s*(?:\{|```)", suffix):
            payload = dict(invalid_payload)

if not isinstance(payload, dict):
    markers = (("SCOPE_BLOCKED", "scope_blocked"), ("BLOCKED", "blocked"),
               ("NEED_GUIDANCE", "guidance"), ("NEED_DECOMPOSITION", "guidance"),
               ("CLOSEOUT_INCOMPLETE", "blocked"), ("CLOSEOUT_REQUIRED", "closeout_required"),
               ("DEAD_END", "blocked"))
    final_outcome = next((val for marker, val in markers if re.search(r"\[" + marker + r"\]", text or full_log)), "failure")
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
               "diagnostic": "AgY final JSON requires a supported outcome"}

payload["actions_used"] = actions
payload["cli_exit_code"] = code
if outcome != "complete":
    payload["outcome"] = outcome
    final_status = "failed" if outcome == "failure" else outcome
else:
    if payload.get("outcome") in ("complete", "no_change", "closeout_required"):
        final_status = "complete"
    else:
        final_status = "failed"

json.dump(payload, open(result, "w", encoding="utf-8"))
json.dump({"status": final_status, "result_location": result, "exit_code": code}, open(status, "w", encoding="utf-8"))
'''
    extra_path = str(Path.home() / ".local/bin")
    current_path = os.environ.get("PATH", "")
    child_path = f"{extra_path}:{current_path}" if extra_path not in current_path else current_path
    child_env = {**request.get("environment", {}),
                 "PATH": child_path,
                 "PYTHONUNBUFFERED": "1",
                 "NIGHTSHIFT_TASK_ID": request.get("task_id", ""),
                 "NIGHTSHIFT_EXECUTION_KIND": request.get("kind", "main"),
                 "MAX_STEPS": str(limit),
                 "AGY_MAX_STEPS": str(limit),
                 "PORT": request.get("environment", {}).get("PORT", "3001"),
                 "TEST_PORT": request.get("environment", {}).get("TEST_PORT", "3001")}
    proc = subprocess.Popen([sys.executable, "-c", runner, json.dumps(argv),
                             request["working_directory"], json.dumps(child_env),
                             str(log_path), str(message_path), str(result_path),
                             str(status_path), str(limit)], stdin=subprocess.PIPE,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=(os.name == "posix"))
    proc.stdin.write(prompt.encode("utf-8"))
    proc.stdin.close()
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
        response({"stopped": True})
        return
    deadline = time.monotonic() + float(request.get("stop_timeout_seconds", 2))
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
            stat = Path(f"/proc/{pid}/stat")
            if stat.exists() and stat.read_text().split()[2] == "Z":
                response({"stopped": True})
                return
        except ProcessLookupError:
            response({"stopped": True})
            return
        time.sleep(0.05)
    response({"stopped": False})


def closeout(request):
    request["kind"] = "closeout"
    request.setdefault("limits", {})["action_limit"] = request["limits"].get("action_limit", 1)
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
        print(f"agy adapter: {error}", file=sys.stderr)
        sys.exit(1)
