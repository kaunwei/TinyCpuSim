#!/usr/bin/env python3
"""TinyCpuSim NightShift Acceptance Test Runner.

Partitions CTest test cases into:
  - hot: The most recent 100 test cases (with unsplit boundary test suite preserved).
  - cold: The remaining test cases for periodic regression.

Supports dynamic test discovery, custom patterns in nightshift/acceptance_config.json,
easy case addition via CLI, and full NightShift framework adapter protocol.
"""

import argparse
import glob
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

PROJECT_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = PROJECT_ROOT / "build"
CONFIG_FILE = PROJECT_ROOT / "nightshift" / "acceptance_config.json"

DEFAULT_CONFIG = {
    "recent_count": 100,
    "keep_suites_together": True,
    "extra_hot_patterns": [],
    "extra_cold_patterns": []
}


def load_config():
    if CONFIG_FILE.is_file():
        try:
            cfg = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
            merged = dict(DEFAULT_CONFIG)
            merged.update(cfg)
            return merged
        except Exception as e:
            print(f"[WARN] Failed to read {CONFIG_FILE}: {e}, using defaults", file=sys.stderr)
    return dict(DEFAULT_CONFIG)


def save_config(cfg):
    CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_FILE.write_text(json.dumps(cfg, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def ensure_build():
    """Ensure build directory and test targets exist."""
    if not (BUILD_DIR / "CTestTestfile.cmake").exists():
        build_script = PROJECT_ROOT / "scripts" / "01_build.sh"
        if build_script.is_file():
            print("[INFO] Building project first...", flush=True)
            subprocess.run([str(build_script)], cwd=str(PROJECT_ROOT), check=True)
        else:
            BUILD_DIR.mkdir(parents=True, exist_ok=True)
            subprocess.run(["cmake", "-B", str(BUILD_DIR), "-S", str(PROJECT_ROOT)], check=True)
            subprocess.run(["cmake", "--build", str(BUILD_DIR), "-j"], check=True)


def get_all_ctest_tests():
    """Query CTest for all registered tests in order."""
    ensure_build()
    cmd = ["ctest", "--test-dir", str(BUILD_DIR), "--show-only=json-v1"]
    res = subprocess.run(cmd, capture_output=True, text=True, check=True)
    data = json.loads(res.stdout)
    tests = data.get("tests", [])
    for idx, t in enumerate(tests):
        t["1based_index"] = idx + 1
    return tests


def resolve_source(test):
    """Map a CTest test entry to its repository source file."""
    cmd = test.get("command", [])
    if not cmd:
        return None
    cmd0 = os.path.basename(cmd[0])
    tname = test.get("name", "")
    tstem = tname.split(".")[0]

    # 1. Python tests
    if cmd0.startswith("python"):
        for arg in cmd[1:]:
            if arg.endswith(".py"):
                if (PROJECT_ROOT / arg).is_file():
                    return arg
                cand = PROJECT_ROOT / "tests" / os.path.basename(arg)
                if cand.is_file():
                    return str(cand.relative_to(PROJECT_ROOT))
            if "test_" in arg:
                cand = PROJECT_ROOT / "tests" / (arg.split(".")[-1] + ".py")
                if cand.is_file():
                    return str(cand.relative_to(PROJECT_ROOT))

    # 2. C++ platform tests
    platform_map = {
        "test_perf_platform": "platforms/01_basic_perf/test_perf_platform.cpp",
        "test_mcu_platform": "platforms/02_basic_mcu/test_mcu_platform.cpp",
        "test_coprocessor_accel": "platforms/03_coprocessor_accel/test_coprocessor_accel.cpp",
    }
    if cmd0 in platform_map:
        return platform_map[cmd0]

    # 3. C++ unit and uarch tests
    candidates = [
        f"tests/{cmd0}.cpp",
        f"tests/uarch/{cmd0}.cpp",
        f"tests/unit/{cmd0}.cpp",
        f"tests/{tstem}.cpp",
        f"tests/uarch/{tstem}.cpp",
        f"tests/unit/{tstem}.cpp",
    ]
    for c in candidates:
        if (PROJECT_ROOT / c).is_file():
            return c
    return None


def partition_tests(tests, cfg):
    """Partition tests into hot and cold sets according to rules."""
    total = len(tests)
    recent_count = cfg.get("recent_count", 100)
    keep_suites_together = cfg.get("keep_suites_together", True)

    if total <= recent_count:
        hot_indices = set(range(total))
        cold_indices = set()
    else:
        cut = total - recent_count
        if keep_suites_together and cut > 0:
            boundary_exe = tests[cut]["command"][0]
            # "沒辦法簡單拆的就別拆放在hot" -> Keep entire boundary suite in hot
            while cut > 0 and tests[cut - 1]["command"][0] == boundary_exe:
                cut -= 1
        cold_indices = set(range(0, cut))
        hot_indices = set(range(cut, total))

    # Apply explicit patterns
    for pat in cfg.get("extra_hot_patterns", []):
        regex = re.compile(pat)
        for idx, t in enumerate(tests):
            if regex.search(t["name"]) or regex.search(t["command"][0]):
                hot_indices.add(idx)
                cold_indices.discard(idx)

    for pat in cfg.get("extra_cold_patterns", []):
        regex = re.compile(pat)
        for idx, t in enumerate(tests):
            if regex.search(t["name"]) or regex.search(t["command"][0]):
                cold_indices.add(idx)
                hot_indices.discard(idx)

    hot_tests = [tests[i] for i in sorted(hot_indices)]
    cold_tests = [tests[i] for i in sorted(cold_indices)]
    return hot_tests, cold_tests


def compute_ctest_i_argument(selected_tests, all_tests):
    """Compute the most compact -I argument for CTest."""
    if not selected_tests:
        return ""
    indices = [t["1based_index"] for t in selected_tests]
    min_idx, max_idx = min(indices), max(indices)
    index_set = set(indices)

    # Check if indices form a contiguous range [min_idx, max_idx]
    if len(indices) == (max_idx - min_idx + 1) and index_set == set(range(min_idx, max_idx + 1)):
        return f"{min_idx},{max_idx}"

    # Contiguous with extras
    # Find largest contiguous chunk
    return f"{min_idx},{max_idx},1," + ",".join(str(i) for i in indices if i < min_idx or i > max_idx)


def run_ctest_subset(selected_tests, all_tests, extra_args=None):
    """Execute selected tests in parallel via CTest."""
    if not selected_tests:
        print("[INFO] No tests selected.", flush=True)
        return 0

    ensure_build()
    i_arg = compute_ctest_i_argument(selected_tests, all_tests)
    jobs = os.environ.get("JOBS", str(os.cpu_count() or 4))

    cmd = [
        "ctest",
        "--test-dir", str(BUILD_DIR),
        "-j", str(jobs),
        "--output-on-failure"
    ]
    if i_arg:
        cmd.extend(["-I", i_arg])
    if extra_args:
        cmd.extend(extra_args)

    print(f"[RUN] Executing {len(selected_tests)} tests via CTest: {' '.join(cmd)}", flush=True)
    res = subprocess.run(cmd, cwd=str(PROJECT_ROOT))
    return res.returncode


def handle_nightshift_protocol(selected_tests):
    """Handle NightShift framework discovery and run phases with TDD evidence."""
    phase = os.environ.get("NIGHTSHIFT_PHASE")
    result_path = os.environ.get("NIGHTSHIFT_RESULT")
    nonce = os.environ.get("NIGHTSHIFT_RUN", "")
    base_commit = os.environ.get("NIGHTSHIFT_BASE", "HEAD")

    if not result_path:
        return None

    cases = []
    for t in selected_tests:
        case_id = t["name"]
        src = resolve_source(t) or "tests/smoke_test.cpp"
        src_path = PROJECT_ROOT / src
        fingerprint = hashlib.sha256(src_path.read_bytes()).hexdigest() if src_path.is_file() else "0" * 64

        # Baseline fingerprint
        baseline = None
        git_show = subprocess.run(["git", "-C", str(PROJECT_ROOT), "show", f"{base_commit}:{src}"],
                                  capture_output=True)
        if git_show.returncode == 0:
            baseline = hashlib.sha256(git_show.stdout).hexdigest()

        case_entry = {
            "id": case_id,
            "source": src,
            "fingerprint": fingerprint,
            "baseline_fingerprint": baseline
        }
        cases.append(case_entry)

    if phase == "discover":
        payload = {"run": nonce, "cases": cases}
        Path(result_path).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        return 0

    if phase == "run":
        # Execute tests and record passed/failed status
        all_tests = get_all_ctest_tests()
        code = run_ctest_subset(selected_tests, all_tests)
        status_val = "passed" if code == 0 else "failed"
        for c in cases:
            c["status"] = status_val
        payload = {"run": nonce, "cases": cases}
        Path(result_path).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        return code

    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["hot", "cold", "all"], default="hot",
                        help="Select test suite to run (default: hot)")
    parser.add_argument("--list", action="store_true",
                        help="List partitioned test counts, boundary suite and case details")
    parser.add_argument("--range", choices=["hot", "cold"],
                        help="Output CTest -I range argument string only (e.g. 267,377)")
    parser.add_argument("--add-hot", metavar="PATTERN",
                        help="Add a regex or name pattern to hot tests in nightshift/acceptance_config.json")
    parser.add_argument("--add-cold", metavar="PATTERN",
                        help="Add a regex or name pattern to cold tests in nightshift/acceptance_config.json")
    parser.add_argument("--count", type=int,
                        help="Override recent test count threshold")
    parser.add_argument("ctest_args", nargs="*",
                        help="Extra arguments passed directly to ctest")

    args = parser.parse_args()
    cfg = load_config()

    if args.count is not None:
        cfg["recent_count"] = args.count

    if args.add_hot:
        if args.add_hot not in cfg["extra_hot_patterns"]:
            cfg["extra_hot_patterns"].append(args.add_hot)
            save_config(cfg)
            print(f"[OK] Added '{args.add_hot}' to extra_hot_patterns in {CONFIG_FILE}")
        return 0

    if args.add_cold:
        if args.add_cold not in cfg["extra_cold_patterns"]:
            cfg["extra_cold_patterns"].append(args.add_cold)
            save_config(cfg)
            print(f"[OK] Added '{args.add_cold}' to extra_cold_patterns in {CONFIG_FILE}")
        return 0

    all_tests = get_all_ctest_tests()
    hot_tests, cold_tests = partition_tests(all_tests, cfg)

    if args.range:
        selected = hot_tests if args.range == "hot" else cold_tests
        print(compute_ctest_i_argument(selected, all_tests))
        return 0

    if args.list:
        print("============================================================")
        print("       TinyCpuSim NightShift Acceptance Test Partition      ")
        print("============================================================")
        print(f"Total CTest Tests:       {len(all_tests)}")
        print(f"Configured Recent Count: {cfg.get('recent_count', 100)}")
        print(f"Keep Suites Together:    {cfg.get('keep_suites_together', True)}")
        print(f"Hot Suite:               {len(hot_tests)} tests (#{hot_tests[0]['1based_index']}..#{hot_tests[-1]['1based_index']})")
        print(f"  First hot test:        {hot_tests[0]['name']}")
        print(f"  Last hot test:         {hot_tests[-1]['name']}")
        print(f"  Boundary Executable:   {os.path.basename(hot_tests[0]['command'][0])}")
        print(f"Cold Suite:              {len(cold_tests)} tests (#{cold_tests[0]['1based_index']}..#{cold_tests[-1]['1based_index']})")
        print(f"  First cold test:       {cold_tests[0]['name']}")
        print(f"  Last cold test:        {cold_tests[-1]['name']}")
        if cfg.get("extra_hot_patterns"):
            print(f"Extra Hot Patterns:      {cfg['extra_hot_patterns']}")
        if cfg.get("extra_cold_patterns"):
            print(f"Extra Cold Patterns:     {cfg['extra_cold_patterns']}")
        print("============================================================")
        return 0

    selected = hot_tests if args.suite == "hot" else (cold_tests if args.suite == "cold" else all_tests)

    # Check NightShift framework environment protocol
    ns_code = handle_nightshift_protocol(selected)
    if ns_code is not None:
        return ns_code

    # Direct CLI execution
    code = run_ctest_subset(selected, all_tests, args.ctest_args)
    return code


if __name__ == "__main__":
    sys.exit(main())
