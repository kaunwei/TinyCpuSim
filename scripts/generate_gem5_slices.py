#!/usr/bin/python3
"""
Generate gem5 Golden Reference Slices for TinyArmSim Benchmarks
Runs gem5 with periodic instruction and tick stat dumps matching TinyCpuSim intervals
and stores multi-dump golden files to tests/golden/gem5/slices/
"""

import os
import sys
import glob
import subprocess
import shutil
import argparse
import concurrent.futures
from typing import Tuple, List, Optional


def _run_single_gem5_slice(args_tuple: Tuple[str, str, str, str, int, int]) -> Tuple[str, bool, str]:
    elf, gem5_bin, runner_script, slices_dir, slice_insts, slice_ticks = args_tuple
    case_name = os.path.splitext(os.path.basename(elf))[0]
    outdir = f"/tmp/gem5_slices_{case_name}"
    if os.path.exists(outdir):
        shutil.rmtree(outdir, ignore_errors=True)

    cmd = [
        gem5_bin,
        f"--outdir={outdir}",
        runner_script,
        "--cpu=o3",
        f"--slice-insts={slice_insts}",
        f"--slice-ticks={slice_ticks}",
        elf
    ]

    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        return case_name, False, f"FAILED (returncode {res.returncode}): {res.stderr[:200]}"

    stats_file = os.path.join(outdir, "stats.txt")
    if os.path.exists(stats_file):
        dest_file = os.path.join(slices_dir, f"{case_name}.stats.txt")
        shutil.copyfile(stats_file, dest_file)
        # Count number of dump blocks
        with open(dest_file, "r") as fp:
            content = fp.read()
        num_dumps = content.count("Begin Simulation Statistics")
        return case_name, True, f"DONE -> {os.path.basename(dest_file)} ({num_dumps} slice dumps)"
    else:
        return case_name, False, "FAILED (stats.txt not generated)"


def main():
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    gem5_bin = os.environ.get("GEM5_BIN", "/home/kw/workspace/gem5/build/ARM/gem5.opt")
    default_runner = os.path.join(root_dir, "scripts", "gem5_slice_runner.py")
    fixtures_dir = os.path.join(root_dir, "tests", "fixtures")
    slices_dir = os.path.join(root_dir, "tests", "golden", "gem5", "slices")

    default_jobs = max(1, (os.cpu_count() or 4) // 2)
    parser = argparse.ArgumentParser(
        description="Generate gem5 Multi-Slice Golden Stats for TinyArmSim Benchmarks"
    )
    parser.add_argument(
        "-j", "--jobs",
        type=int,
        default=default_jobs,
        help=f"Parallel worker threads/processes (default: {default_jobs})"
    )
    parser.add_argument(
        "--slice-insts",
        type=int,
        default=1000,
        help="Periodic instruction interval for stat dumps (default: 1000)"
    )
    parser.add_argument(
        "--slice-ticks",
        type=int,
        default=0,
        help="Periodic tick interval for stat dumps (default: 0, disabled if 0)"
    )
    parser.add_argument(
        "--gem5-bin",
        type=str,
        default=gem5_bin,
        help=f"Path to gem5 binary (default: {gem5_bin})"
    )
    parser.add_argument(
        "--runner-script",
        type=str,
        default=default_runner,
        help=f"Path to gem5 slice runner script (default: {default_runner})"
    )
    parser.add_argument(
        "--fixtures-dir",
        type=str,
        default=fixtures_dir,
        help=f"Directory containing ELF benchmark fixtures (default: {fixtures_dir})"
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default=slices_dir,
        help=f"Target directory for golden slice stats (default: {slices_dir})"
    )
    parser.add_argument(
        "--fixtures",
        nargs="*",
        help="Optional specific fixture names or patterns to run (e.g. branch_loop sum_array)"
    )
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    if args.fixtures:
        elf_files = []
        for f in args.fixtures:
            cand = f if f.endswith(".elf") else f"{f}.elf"
            full_path = os.path.join(args.fixtures_dir, cand) if not os.path.isabs(cand) else cand
            if os.path.exists(full_path):
                elf_files.append(full_path)
            else:
                print(f"Warning: Fixture {f} not found at {full_path}")
        elf_files = sorted(list(set(elf_files)))
    else:
        elf_files = sorted(glob.glob(os.path.join(args.fixtures_dir, "*.elf")))

    if not elf_files:
        print(f"No ELF files found in {args.fixtures_dir}. Please build fixtures first.")
        sys.exit(1)

    print(f"Generating gem5 multi-slice golden stats for {len(elf_files)} fixtures...")
    print(f"Interval: {args.slice_insts} instructions / {args.slice_ticks} ticks")
    print(f"Output directory: {args.output_dir}")
    print(f"gem5 Binary: {args.gem5_bin}")
    print(f"Parallel Workers: {args.jobs}")
    print("-" * 65)

    if not os.path.exists(args.gem5_bin):
        print(f"Warning: gem5 binary not found at {args.gem5_bin}.")
        print("Ensure GEM5_BIN is built and configured before running live slice generations.")

    task_args = [
        (elf, args.gem5_bin, args.runner_script, args.output_dir, args.slice_insts, args.slice_ticks)
        for elf in elf_files
    ]

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        results = list(executor.map(_run_single_gem5_slice, task_args))

    for case_name, ok, msg in results:
        print(f"[{case_name}]: {msg}")

    print("-" * 65)
    all_ok = all(ok for _, ok, _ in results)
    if all_ok:
        print("All golden slice statistics generated successfully.")
    else:
        print("Completed with some failures.")


if __name__ == "__main__":
    main()
