#!/usr/bin/env python3
"""Compatibility adapter for executable NightShift wrappers."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def emit(value):
    print(json.dumps(value, ensure_ascii=False))


def request():
    value = json.load(sys.stdin)
    if value.get("protocol_version") != 1:
        raise ValueError("protocol_version 1 is required")
    return value


def get_config(value):
    overrides = value.get("overrides", {})
    return {"protocol_version": 1, "initial_score": int(overrides.get("initial_score", 3)),
            "limits": {"poll_interval_seconds": .25, "callback_timeout_seconds": 30},
            "configuration": {"executable": overrides.get("command", overrides.get("executable", "")),
                              "args": overrides.get("args", []),
                              "model": overrides.get("model", ""),
                              "effort": overrides.get("effort", ""),
                              "env": overrides.get("env", {})}}


def start(value):
    cfg = value["configuration"]
    if not cfg.get("executable"):
        raise ValueError("generic adapter requires overrides.executable; see PORTING_GUIDE.md")
    if not isinstance(cfg.get("args"), list) or not all(isinstance(arg, str) for arg in cfg["args"]):
        raise ValueError("generic adapter overrides.args must be a string array")
    if not isinstance(cfg.get("env"), dict) or not all(isinstance(k, str) and isinstance(v, str)
                                                        for k, v in cfg["env"].items()):
        raise ValueError("generic adapter overrides.env must map strings to strings")
    output = Path(value["output_dir"]); output.mkdir(parents=True, exist_ok=True)
    status = output / "status.json"; log = output / "wrapper.log"
    result = Path(value["result_path"]); prompt = Path(value["prompt_path"])
    environment = {**value.get("environment", {}), **cfg["env"],
                   "NIGHTSHIFT_PROMPT_FILE": str(prompt.resolve()),
                   "NIGHTSHIFT_RESULT_FILE": str(result.resolve()),
                   "NIGHTSHIFT_TASK_ID": str(value.get("task_id", "")),
                   "NIGHTSHIFT_MODEL": str(cfg.get("model", "")),
                   "NIGHTSHIFT_EFFORT": str(cfg.get("effort", "")),
                   "NIGHTSHIFT_EXECUTION_KIND": str(value.get("kind", "main")),
                   "NIGHTSHIFT_ACTION_LIMIT": str(value.get("limits", {}).get("action_limit", "")),
                   "NIGHTSHIFT_EXECUTION_TIMEOUT": str(value.get("limits", {}).get("execution_timeout_seconds", "")),
                   "PORT": str(value.get("environment", {}).get("PORT", "3001")),
                   "TEST_PORT": str(value.get("environment", {}).get("TEST_PORT", "3001"))}
    runner = r'''import json, os, subprocess, sys
argv, cwd, env, prompt, result, status, log = sys.argv[1:]
code = subprocess.run(json.loads(argv) + [open(prompt, encoding="utf-8").read()], cwd=cwd,
    env={**os.environ, **json.loads(env)}, stdout=open(log,"w"), stderr=subprocess.STDOUT,
    text=True, check=False).returncode
try:
    payload=json.load(open(result,encoding="utf-8"))
    if not isinstance(payload,dict) or payload.get("outcome") not in ("complete","no_change","closeout_required","blocked","scope_blocked","guidance","failure"):
        raise ValueError("unsupported outcome")
    outcome=payload["outcome"]
except (OSError,ValueError,TypeError) as error:
    payload={"outcome":"failure","diagnostic":f"wrapper must write a structured JSON result to NIGHTSHIFT_RESULT_FILE: {error}"}
    json.dump(payload,open(result,"w",encoding="utf-8")); outcome="failure"
if code and outcome in ("complete","no_change"): outcome="failure"
diagnostic=open(log,encoding="utf-8",errors="replace").read()
state="execution_timeout" if "print timeout after" in diagnostic.lower() else outcome
json.dump({"status":state,"result_location":result,"exit_code":code},open(status,"w",encoding="utf-8"))
'''
    argv = [cfg["executable"], *cfg["args"]]
    proc = subprocess.Popen([sys.executable, "-c", runner, json.dumps(argv),
                             value["working_directory"], json.dumps(environment),
                             str(prompt), str(result), str(status), str(log)],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, start_new_session=(os.name == "posix"))
    emit({"execution_id": str(proc.pid), "status_location": str(status)})


def main():
    callback = sys.argv[-1]; value = request()
    if callback == "get_config":
        emit(get_config(value))
    elif callback in ("start", "closeout"):
        start(value)
    elif callback == "status":
        path = Path(value["status_location"])
        emit(json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {"status":"running"})
    elif callback == "cancel":
        pid = int(value["execution_id"])
        try:
            os.killpg(pid, signal.SIGTERM) if os.name == "posix" else os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            emit({"stopped":True}); return
        deadline=time.monotonic()+float(value.get("stop_timeout_seconds",2))
        while time.monotonic()<deadline:
            try:
                os.kill(pid,0)
                stat=Path(f"/proc/{pid}/stat")
                if stat.exists() and stat.read_text().split()[2]=="Z":
                    emit({"stopped":True}); return
            except ProcessLookupError:
                emit({"stopped":True}); return
            time.sleep(.05)
        emit({"stopped":False})
    else:
        raise ValueError(f"unsupported callback: {callback}")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"generic adapter: {error}", file=sys.stderr); sys.exit(1)
