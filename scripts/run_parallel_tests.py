#!/usr/bin/python3
"""Isolated parallel configuration test runner with bounded concurrency.

Uses ResourcePool to limit concurrent CPU slots and memory usage across
independent configuration test jobs, ensuring isolated scratch roots and
clean process-group cleanup on failure or timeout.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pathlib
import signal
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from typing import List, Optional

try:
    from tests.fw_resource_support import ResourceBudget, ResourcePool
except ImportError:
    try:
        from fw_resource_support import ResourceBudget, ResourcePool  # type: ignore
    except ImportError:
        _root = str(pathlib.Path(__file__).resolve().parent.parent)
        if _root not in sys.path:
            sys.path.insert(0, _root)
        from tests.fw_resource_support import ResourceBudget, ResourcePool


DEFAULT_PYTHON: str = "/usr/bin/python3"
DEFAULT_CONFIGS = ["debug", "release", "guest-o0", "guest-o2"]
DEFAULT_JOBS = 8
DEFAULT_MEMORY_MB = 8192
DEFAULT_TIMEOUT_SEC = 120.0
DEFAULT_CONFIG_BUDGET = ResourceBudget(cpu_slots=1, memory_mib=512)


@dataclass
class ConfigResult:
    config: str
    exit_code: int
    elapsed_seconds: float
    stdout: str
    stderr: str
    max_rss_kib: Optional[int] = None
    error: Optional[str] = None


@dataclass
class RunnerSummary:
    total_configs: int
    passed_configs: int
    failed_configs: int
    wall_time_seconds: float
    sequential: bool
    results: List[ConfigResult] = field(default_factory=list)


def resolve_config_command(config: str, scratch_dir: pathlib.Path) -> List[str]:
    """Return command vector for the given configuration using canonical python."""
    py = DEFAULT_PYTHON if os.path.exists(DEFAULT_PYTHON) else sys.executable
    if config == "debug":
        return [py, "-m", "unittest", "tests.test_fw_config", "tests.test_fw_template"]
    elif config == "release":
        return [py, "-m", "unittest", "tests.test_fw_build_matrix"]
    elif config == "guest-o0":
        return [py, "-m", "unittest", "tests.test_fw_quiet"]
    elif config == "guest-o2":
        return [py, "-m", "unittest", "tests.test_fw_scheduler"]
    else:
        return [py, "-c", f"print('Config {config} executed in {scratch_dir}')"]


def _terminate_process_group(pgid: int) -> None:
    """Terminate process group with SIGTERM followed by SIGKILL."""
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(pgid, sig)
        except OSError:
            return
        time.sleep(0.2)


def run_single_config(
    config: str,
    scratch_root: pathlib.Path,
    pool: Optional[ResourcePool] = None,
    timeout: float = DEFAULT_TIMEOUT_SEC,
    budget: ResourceBudget = DEFAULT_CONFIG_BUDGET,
    custom_cmd: Optional[List[str]] = None,
) -> ConfigResult:
    """Execute a single configuration within isolated directory and pool budget."""
    scratch_dir = scratch_root / config
    scratch_dir.mkdir(parents=True, exist_ok=True)

    cmd = custom_cmd if custom_cmd is not None else resolve_config_command(config, scratch_dir)
    env = os.environ.copy()
    _repo_root_str = str(pathlib.Path(__file__).resolve().parent.parent)
    if "PYTHONPATH" in env:
        env["PYTHONPATH"] = f"{_repo_root_str}:{env['PYTHONPATH']}"
    else:
        env["PYTHONPATH"] = _repo_root_str
    env["PARALLEL_CONFIG_NAME"] = config
    env["PARALLEL_CONFIG_SCRATCH"] = str(scratch_dir)

    start_time = time.monotonic()
    stdout_data = ""
    stderr_data = ""
    exit_code = -1
    err_msg: Optional[str] = None
    max_rss: Optional[int] = None

    def execute_proc() -> int:
        nonlocal stdout_data, stderr_data
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            cwd=str(scratch_dir),
            env=env,
            preexec_fn=os.setsid if hasattr(os, "setsid") else None,
        )
        try:
            out, err = proc.communicate(timeout=timeout)
            stdout_data = out
            stderr_data = err
            return proc.returncode
        except subprocess.TimeoutExpired:
            if hasattr(os, "killpg") and hasattr(os, "getpgid"):
                try:
                    _terminate_process_group(os.getpgid(proc.pid))
                except OSError:
                    pass
            else:
                proc.kill()
            out, err = proc.communicate()
            stdout_data = out
            stderr_data = (err or "") + f"\n[TIMEOUT] Process killed after {timeout}s timeout"
            return 124

    try:
        if pool is not None:
            with pool.reserve(cpu_slots=budget.cpu_slots, memory_mib=budget.memory_mib, timeout=timeout):
                exit_code = execute_proc()
        else:
            exit_code = execute_proc()
    except Exception as exc:
        err_msg = str(exc)
        exit_code = 1
        stderr_data += f"\n[EXECUTION ERROR] {exc}"

    elapsed = time.monotonic() - start_time
    # Capture peak memory RSS if available via /proc or resource
    try:
        import resource
        rusage = resource.getrusage(resource.RUSAGE_CHILDREN)
        max_rss = rusage.ru_maxrss
    except Exception:
        max_rss = None

    return ConfigResult(
        config=config,
        exit_code=exit_code,
        elapsed_seconds=round(elapsed, 3),
        stdout=stdout_data,
        stderr=stderr_data,
        max_rss_kib=max_rss,
        error=err_msg,
    )


def run_parallel_suite(
    configs: List[str],
    jobs: int = DEFAULT_JOBS,
    memory_mb: int = DEFAULT_MEMORY_MB,
    scratch_root: Optional[pathlib.Path] = None,
    timeout: float = DEFAULT_TIMEOUT_SEC,
    sequential: bool = False,
    budget: ResourceBudget = DEFAULT_CONFIG_BUDGET,
    custom_cmd: Optional[List[str]] = None,
) -> RunnerSummary:
    """Run all specified configurations concurrently or sequentially."""
    if scratch_root is None:
        scratch_root = pathlib.Path.cwd() / ".cache" / "parallel_runs"
    scratch_root.mkdir(parents=True, exist_ok=True)

    pool_dir = scratch_root / ".pool"
    pool = ResourcePool(
        directory=pool_dir,
        budget=ResourceBudget(cpu_slots=jobs, memory_mib=memory_mb),
    )

    start_wall = time.monotonic()
    results: List[ConfigResult] = []

    if sequential or jobs <= 1:
        for cfg in configs:
            res = run_single_config(
                config=cfg,
                scratch_root=scratch_root,
                pool=pool,
                timeout=timeout,
                budget=budget,
                custom_cmd=custom_cmd,
            )
            results.append(res)
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
            future_to_cfg = {
                executor.submit(
                    run_single_config,
                    cfg,
                    scratch_root,
                    pool,
                    timeout,
                    budget,
                    custom_cmd,
                ): cfg
                for cfg in configs
            }
            for future in concurrent.futures.as_completed(future_to_cfg):
                results.append(future.result())

    wall_time = round(time.monotonic() - start_wall, 3)
    results.sort(key=lambda r: r.config)
    passed = sum(1 for r in results if r.exit_code == 0)
    failed = len(results) - passed

    return RunnerSummary(
        total_configs=len(configs),
        passed_configs=passed,
        failed_configs=failed,
        wall_time_seconds=wall_time,
        sequential=sequential,
        results=results,
    )


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Isolated parallel configuration test runner.")
    parser.add_argument(
        "--configs",
        nargs="+",
        default=DEFAULT_CONFIGS,
        help="List of configuration names to run.",
    )
    parser.add_argument("--jobs", "-j", type=int, default=DEFAULT_JOBS, help="Max CPU concurrency.")
    parser.add_argument("--memory-mb", type=int, default=DEFAULT_MEMORY_MB, help="Memory pool budget in MiB.")
    parser.add_argument(
        "--scratch-root",
        type=pathlib.Path,
        default=pathlib.Path.cwd() / ".cache" / "parallel_runs",
        help="Isolated scratch root directory.",
    )
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SEC, help="Timeout in seconds per job.")
    parser.add_argument("--sequential", action="store_true", help="Force sequential execution.")
    parser.add_argument("--json", action="store_true", help="Output summary as JSON.")

    args = parser.parse_args(argv)

    summary = run_parallel_suite(
        configs=args.configs,
        jobs=args.jobs,
        memory_mb=args.memory_mb,
        scratch_root=args.scratch_root,
        timeout=args.timeout,
        sequential=args.sequential,
    )

    if args.json:
        print(json.dumps(asdict(summary), indent=2))
    else:
        print(f"Parallel Test Summary: {summary.passed_configs}/{summary.total_configs} passed in {summary.wall_time_seconds}s (sequential={summary.sequential})")
        for res in summary.results:
            status = "PASS" if res.exit_code == 0 else f"FAIL(exit={res.exit_code})"
            rss_info = f", rss={res.max_rss_kib}KiB" if res.max_rss_kib else ""
            print(f"  [{status}] {res.config} ({res.elapsed_seconds}s{rss_info})")

    return 0 if summary.failed_configs == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
