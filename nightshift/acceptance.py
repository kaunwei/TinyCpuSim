"""Project-owned tests; runtime-owned evidence and durable lifecycle state."""

from contextlib import contextmanager
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import uuid


class GateError(ValueError):
    pass


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          capture_output=True).stdout.decode("utf-8", "replace").strip()


def state_dir(root):
    try:
        path = Path(git(root, "rev-parse", "--git-path", "nightshift"))
        path = path if path.is_absolute() else Path(root) / path
    except Exception:
        path = Path(root) / "nightshift"
    path.mkdir(parents=True, exist_ok=True)
    return path


def save(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex)
    try:
        temporary.write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def load(path, default=None):
    return json.loads(Path(path).read_text()) if Path(path).exists() else default


@contextmanager
def workspace(root):
    # OS locks release on exit/crash; all Worker IDs share this checkout lock.
    import fcntl
    with (state_dir(root) / "workspace.lock").open("a") as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise GateError("another runtime owns this project workspace") from error
        try:
            yield
        finally:
            fcntl.flock(stream, fcntl.LOCK_UN)


def config(root):
    path = Path(root) / "nightshift/config.json"
    value = None
    if path.is_file() and not path.is_symlink():
        raw = load(path)
        if isinstance(raw, dict):
            if "acceptance" in raw and isinstance(raw["acceptance"], dict):
                value = dict(raw["acceptance"])
                value.setdefault("version", raw.get("version", 1))
            elif "hot" in raw or "cold" in raw:
                value = dict(raw)
    if value is None:
        path = Path(root) / "nightshift/acceptance.json"
        if path.is_file() and not path.is_symlink():
            value = load(path)
    if value is None:
        raise GateError("migration required: configure nightshift/config.json or nightshift/acceptance.json; see acceptance.md")
    if not isinstance(value, dict) or value.get("version") != 1:
        raise GateError("acceptance config requires version 1")
    timeout = value.get("timeout", 180)
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or timeout <= 0:
        raise GateError("acceptance timeout must be finite and positive")
    interval = value.get("cold_every")
    if interval is not None and (type(interval) is not int or interval < 1):
        raise GateError("cold_every must be a positive integer or absent")
    wiring = value.get("wiring", [])
    if not isinstance(wiring, list) or any(not safe_path(item) for item in wiring):
        raise GateError("wiring must list exact project-relative files")
    return value


def safe_path(name):
    return (isinstance(name, str) and bool(name) and not Path(name).is_absolute()
            and ".." not in Path(name).parts and ".git" not in Path(name).parts)


def command(value, name):
    text = value.get(name)
    if not isinstance(text, str) or not text.strip():
        raise GateError(f"missing project {name} command")
    return text


class AcceptanceRunner:
    """Abstract interface defining project hot and cold acceptance."""

    def run_hot(self, args, result, run):
        raise NotImplementedError

    def run_cold(self, root, run):
        raise NotImplementedError


class DefaultAcceptanceRunner(AcceptanceRunner):
    """Default runner executing configured commands with TDD evidence checking."""

    def invoke(self, root, cfg, name, phase, base, run, task="", required=()):
        nonce = uuid.uuid4().hex
        output = state_dir(root) / "evidence" / f"{nonce}.json"
        output.parent.mkdir(parents=True, exist_ok=True)
        environment = {"NIGHTSHIFT_PHASE": phase, "NIGHTSHIFT_RESULT": str(output.resolve()),
                       "NIGHTSHIFT_RUN": nonce, "NIGHTSHIFT_BASE": base,
                       "NIGHTSHIFT_TASK": task, "NIGHTSHIFT_REQUIRED": json.dumps(list(required)),
                       "PYTHONDONTWRITEBYTECODE": "1", "PYTHONPYCACHEPREFIX": str(output.with_suffix(".cache"))}
        # Discover permanent membership with no ticket context.
        if phase == "discover":
            environment.update(NIGHTSHIFT_TASK="", NIGHTSHIFT_REQUIRED="[]")
        prefix = "env " + " ".join(shlex.quote(f"{key}={val}") for key, val in environment.items())
        actual = f"{prefix} bash -c {shlex.quote(command(cfg, name))}"
        code, text = run(actual, str(root), "3001", cfg.get("timeout", 180))
        evidence = {"phase": phase, "command": command(cfg, name), "exit": code,
                    "output": text[-16384:], "run": nonce}
        save(output.with_suffix(".log.json"), evidence)
        if code:
            raise GateError(f"{name}/{phase} exited {code}\n{text[-16384:]}")
        data = load(output)
        if not isinstance(data, dict) or data.get("run") != nonce or not isinstance(data.get("cases"), list):
            raise GateError(f"{name}/{phase}: missing or stale case evidence: {output}")
        cases = {}
        for case in data["cases"]:
            if not isinstance(case, dict) or not isinstance(case.get("id"), str) or not case["id"] or case["id"] in cases:
                raise GateError("case evidence requires unique nonempty IDs")
            cases[case["id"]] = case
        return cases, evidence

    def run_hot(self, args, result, run):
        root = Path(args.work_dir)
        print(f"\n[HOT_GATE] >>> Starting Hot Acceptance Gate for task: {args.task_id}", flush=True)
        cfg = config(root)
        config_file = "nightshift/config.json" if (root / "nightshift/config.json").is_file() else "nightshift/acceptance.json"
        for name in [config_file, *cfg.get("wiring", [])]:
            if subprocess.run(["git", "-C", str(root), "check-ignore", "-q", "--", name]).returncode == 0:
                raise GateError(f"persistent hot wiring must be delivered in Git: {name}")
        declarations = result.get("tdd_cases")
        if not isinstance(declarations, list) or not declarations:
            raise GateError("development handoff requires nonempty tdd_cases")
        declared = {}
        for item in declarations:
            if (not isinstance(item, dict) or not isinstance(item.get("id"), str)
                    or not item["id"] or not safe_path(item.get("source")) or item["id"] in declared):
                raise GateError("tdd_cases requires unique IDs and relative source paths")
            declared[item["id"]] = item["source"]
        print(f"[HOT_GATE] Discovering permanent test cases (baseline={args.base[:8]})...", flush=True)
        permanent, discovery = self.invoke(root, cfg, "hot", "discover", args.base, run)
        for identity, source in declared.items():
            case = permanent.get(identity, {})
            fingerprint = case.get("fingerprint", "")
            baseline = case.get("baseline_fingerprint")
            path = root / source
            if (not path.resolve().is_relative_to(root.resolve()) or not path.is_file()
                    or path.is_symlink() or case.get("source") != source):
                raise GateError(f"TDD case source not found: {identity}")
            if (not isinstance(fingerprint, str) or not re.fullmatch(r"[a-f0-9]{64}", fingerprint)
                    or (baseline is not None and (not isinstance(baseline, str) or not re.fullmatch(r"[a-f0-9]{64}", baseline)))
                    or "baseline_fingerprint" not in case or baseline == fingerprint):
                raise GateError(f"TDD case unchanged or source evidence unsupported: {identity}")
            old = subprocess.run(["git", "-C", str(root), "show", f"{args.base}:{source}"], capture_output=True)
            if old.returncode == 0 and old.stdout == path.read_bytes():
                raise GateError(f"declared TDD source unchanged: {source}")
            ignored = subprocess.run(["git", "-C", str(root), "check-ignore", "-q", "--", source])
            if ignored.returncode == 0:
                raise GateError(f"TDD source must be delivered in Git: {source}")
        required = set(declared)
        explicit = getattr(args, "required_cases", [])
        if isinstance(explicit, str):
            explicit = [part.strip() for part in explicit.split(",") if part.strip()]
        required.update(explicit)
        print(f"[HOT_GATE] Running hot test suite with {len(required)} required TDD cases...", flush=True)
        executed, execution = self.invoke(root, cfg, "hot", "run", args.base, run, args.task_id, sorted(required))
        if not executed or any(case.get("status") != "passed" for case in executed.values()):
            raise GateError("hot contains failed, skipped or invalid case results")
        for identity in required:
            if executed.get(identity, {}).get("status") != "passed":
                raise GateError(f"required case did not execute and pass: {identity}")
            if identity in declared and any(executed[identity].get(field) != permanent[identity].get(field)
                                            for field in ("source", "fingerprint")):
                raise GateError(f"case evidence changed between discovery and execution: {identity}")
        evidence = [discovery, execution]
        if args.verify and args.verify.lower() not in ("none", "report_only"):
            print(f"[HOT_GATE] Running task verify command: {args.verify}...", flush=True)
            code, output = run(args.verify, str(root), args.worker_port, cfg.get("timeout", 180))
            evidence.append({"phase": "task", "command": args.verify, "exit": code, "output": output[-16384:]})
            if code:
                raise GateError(f"task VERIFY exited {code}\n{output[-16384:]}")
        print(f"[HOT_GATE] <<< Hot Acceptance Gate PASSED cleanly for {args.task_id}!\n", flush=True)
        return {"cases": sorted(executed), "evidence": evidence}

    def run_cold(self, root, run):
        print(f"\n[COLD_REGRESSION] >>> Starting Periodic Cold Regression Suite...", flush=True)
        cfg = config(root)
        head = git(root, "rev-parse", "HEAD")
        path = state_dir(root) / "cold.json"
        state = load(path, {"commits": [], "pending": False})
        if state.get("passed") == head:
            state.update(commits=[], pending=False)
            save(path, state)
            print(f"[COLD_REGRESSION] Already passed for HEAD {head[:8]}.", flush=True)
            return
        ignored = re.compile(r"(?:^|/)(?:nightshift/)?(?:progress\.log|tasks\.txt|tasks\.done|worker\..*\.status|\.preview|\.reports|\.worker.*|\.scratch)")
        def product_status():
            raw = git(root, "status", "--porcelain", "--untracked-files=all")
            return [line for line in raw.splitlines() if line and not ignored.search(line[3:])]

        before = product_status()
        if before:
            raise GateError("cold requires a clean product tree")
        print(f"[COLD_REGRESSION] Running cold test command at HEAD {head[:8]}...", flush=True)
        code, output = run(command(cfg, "cold"), str(root), "3001", cfg.get("timeout", 180))
        after = product_status()
        if code or before != after or git(root, "rev-parse", "HEAD") != head:
            raise GateError(f"cold failed or changed product files (exit {code}) at {head}\n{output[-16384:]}")
        state.update(passed=head, commits=[], pending=False)
        save(path, state)
        print(f"[COLD_REGRESSION] <<< Cold Regression Suite PASSED cleanly for HEAD {head[:8]}!\n", flush=True)


def get_runner(root, cfg=None):
    """Resolve the acceptance runner for the project.

    A project can customize how hot and cold are executed by:
    1. Specifying 'runner' in nightshift/acceptance.json as 'module:Class' or 'module:function'.
    2. Placing nightshift/acceptance_runner.py in the project, exporting 'get_runner()' or 'Runner'.
    """
    if cfg is None:
        try:
            cfg = config(root)
        except Exception:
            cfg = {}
    runner_spec = cfg.get("runner")
    if runner_spec and isinstance(runner_spec, str):
        if ":" in runner_spec:
            mod_name, attr_name = runner_spec.split(":", 1)
        else:
            mod_name, attr_name = runner_spec, "Runner"
        if str(root) not in sys.path:
            sys.path.insert(0, str(root))
        import importlib
        mod = importlib.import_module(mod_name)
        cls_or_func = getattr(mod, attr_name)
        return cls_or_func() if isinstance(cls_or_func, type) else cls_or_func(root, cfg)

    custom_hook = Path(root) / "nightshift/acceptance_runner.py"
    if custom_hook.is_file():
        import importlib.util
        spec = importlib.util.spec_from_file_location("custom_acceptance_runner", str(custom_hook))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        if hasattr(mod, "get_runner"):
            return mod.get_runner(root, cfg)
        if hasattr(mod, "Runner"):
            return mod.Runner() if isinstance(mod.Runner, type) else mod.Runner
    return DefaultAcceptanceRunner()


def invoke(root, cfg, name, phase, base, run, task="", required=()):
    return DefaultAcceptanceRunner().invoke(root, cfg, name, phase, base, run, task, required)


def clean(root):
    """Authorized clean-start policy. Never discard commits or ignored files."""
    preserved = {}
    for name in ("tasks.txt", "nightshift/tasks.txt", "tasks.done", "nightshift/tasks.done",
                 "progress.log", "nightshift/progress.log",
                 "nightshift/config.json", "nightshift/acceptance.json",
                 "nightshift/code-index.toml", "nightshift/adapters/agents.json",
                 "nightshift/PORTING_GUIDE.md", "nightshift/.ported"):
        target = Path(root) / name
        if target.is_file():
            preserved[name] = target.read_bytes()
    git(root, "reset", "--hard", "HEAD")
    git(root, "clean", "-fd", "-e", ".scratch", "-e", ".worker*", "-e", "nightshift/",
        "-e", ".agents/", "-e", ".skills/", "-e", ".gitignore")
    for name, content in preserved.items():
        (Path(root) / name).write_bytes(content)
    status_raw = git(root, "status", "--porcelain", "--untracked-files=all")
    if status_raw:
        lines = [line for line in status_raw.splitlines()
                 if not any(name in line for name in preserved)
                 and not line[3:].startswith("nightshift/")
                 and not line[3:].startswith(".agents/")
                 and not line[3:].startswith(".skills/")
                 and not line[3:] == ".gitignore"]
        if lines:
            raise GateError("clean-start failed; Worker not launched")


def key(task_id, title):
    return hashlib.sha256(f"{task_id}\n{title}".encode()).hexdigest()


def record_path(args):
    return state_dir(args.work_dir) / "tasks" / (key(args.task_id, args.title) + ".json")


def pause(root, task, detail):
    path = state_dir(root) / "paused.json"
    if not path.exists():
        save(path, {"task": task, "detail": detail})
        print(f"[PLANNER_REQUIRED] {task}: {detail}", flush=True)


def schedule_cold(root, commit, tasks, insert):
    try:
        cfg = config(root)
    except GateError:
        return
    interval = cfg.get("cold_every")
    if interval is None:
        return
    path = state_dir(root) / "cold.json"
    state = load(path, {"commits": [], "pending": False})
    # A ledger survives completed intervals so replay cannot count a commit twice.
    counted = state.setdefault("counted", [])
    if commit and commit not in counted:
        counted.append(commit)
        state["commits"].append(commit)
    if len(state["commits"]) >= interval or state["pending"]:
        state["pending"] = True
        save(path, state)
        insert(tasks, "TASK-COLD [COLD] | LEVEL: 1 | ACTION: project cold")
    save(path, state)


def hot(args, result, run):
    root = Path(args.work_dir)
    cfg = config(root)
    runner = get_runner(root, cfg)
    return runner.run_hot(args, result, run)


def cold(root, run):
    root = Path(root)
    cfg = config(root)
    runner = get_runner(root, cfg)
    return runner.run_cold(root, run)
