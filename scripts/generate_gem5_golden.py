#!/usr/bin/python3
"""
Generate gem5 Golden Reference Stats for TinyArmSim Benchmarks
Runs gem5 once per test fixture in parallel and stores the golden stats in tests/golden/gem5/
"""

import os
import sys
import glob
import subprocess
import shutil
import argparse
import concurrent.futures

def _run_single_gem5_golden(args_tuple):
    elf, gem5_bin, gem5_config, golden_dir = args_tuple
    case_name = os.path.splitext(os.path.basename(elf))[0]
    outdir = f"/tmp/gem5_golden_{case_name}"
    if os.path.exists(outdir):
        shutil.rmtree(outdir, ignore_errors=True)
        
    cmd = [
        gem5_bin,
        f"--outdir={outdir}",
        gem5_config,
        "--cpu=o3",
        elf
    ]
    
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        return case_name, False, f"FAILED (returncode {res.returncode}): {res.stderr[:200]}"
        
    stats_file = os.path.join(outdir, "stats.txt")
    if os.path.exists(stats_file):
        dest_file = os.path.join(golden_dir, f"{case_name}.stats.txt")
        shutil.copyfile(stats_file, dest_file)
        return case_name, True, f"DONE -> {os.path.basename(dest_file)}"
    else:
        return case_name, False, "FAILED (stats.txt not generated)"

def main():
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    gem5_bin = os.environ.get("GEM5_BIN", "/home/kw/workspace/gem5/build/ARM/gem5.opt")
    gem5_config = os.environ.get("GEM5_CONFIG", "/home/kw/workspace/gem5/configs/example/arm/starter_se.py")
    fixtures_dir = os.path.join(root_dir, "tests", "fixtures")
    golden_dir = os.path.join(root_dir, "tests", "golden", "gem5")
    
    default_jobs = max(1, (os.cpu_count() or 4) // 2)
    parser = argparse.ArgumentParser(description="Generate gem5 Golden Reference Stats for TinyArmSim Benchmarks")
    parser.add_argument("-j", "--jobs", type=int, default=default_jobs, help=f"Parallel worker threads/processes (default: {default_jobs}, half of CPU cores)")
    args = parser.parse_args()

    if not os.path.exists(gem5_bin):
        print(f"Error: gem5 binary not found at {gem5_bin}")
        sys.exit(1)
        
    os.makedirs(golden_dir, exist_ok=True)
    
    elf_files = sorted(glob.glob(os.path.join(fixtures_dir, "*.elf")))
    if not elf_files:
        print(f"No ELF files found in {fixtures_dir}. Please build fixtures first.")
        sys.exit(1)
        
    print(f"Generating gem5 golden stats for {len(elf_files)} fixtures...")
    print(f"Output directory: {golden_dir}")
    print(f"Parallel Workers: {args.jobs} (half of CPU cores)")
    print("-" * 65)

    task_args = [(elf, gem5_bin, gem5_config, golden_dir) for elf in elf_files]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        results = list(executor.map(_run_single_gem5_golden, task_args))

    for case_name, ok, msg in results:
        print(f"[{case_name}]: {msg}")

    print("-" * 65)
    print("All golden reference statistics generated successfully.")

if __name__ == "__main__":
    main()
