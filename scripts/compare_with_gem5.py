#!/usr/bin/python3
"""
TinyArmSim vs gem5 Golden Reference Accuracy Comparator
Parses TinyArmSim performance report and gem5 stats.txt, calculating precision deltas.
Supports both single file comparison and batch regression against tests/golden/gem5/.
"""

import sys
import re
import os
import glob
import subprocess

def parse_tinysim_stats(filename_or_content):
    stats = {}
    if os.path.exists(filename_or_content):
        with open(filename_or_content, 'r') as f:
            content = f.read()
    else:
        content = filename_or_content
    
    m = re.search(r'Simulated Total Cycles:\s+(\d+)', content)
    if m: stats['cycles'] = int(m.group(1))
    
    m = re.search(r'Total Committed Insts:\s+(\d+)', content)
    if m: stats['insts'] = int(m.group(1))
    
    m = re.search(r'Aggregate Throughput \(IPC\):\s*([\d\.]+)', content)
    if m: stats['ipc'] = float(m.group(1))
    
    m = re.search(r'L1I Cache Accesses:\s+\d+\s+\(Hit Rate:\s*([\d\.]+)%\)', content)
    if m: stats['l1i_hit_rate'] = float(m.group(1))

    m = re.search(r'L1D Cache Accesses:\s+\d+\s+\(Hit Rate:\s*([\d\.]+)%\)', content)
    if m: stats['l1d_hit_rate'] = float(m.group(1))

    m = re.search(r'Branch Predictions:\s+\d+\s+\(Accuracy:\s*([\d\.]+)%\)', content)
    if m: stats['branch_acc'] = float(m.group(1))
    
    return stats

def parse_gem5_stats(filename):
    stats = {}
    if not os.path.exists(filename):
        return stats
    with open(filename, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'): continue
            parts = line.split()
            if len(parts) >= 2:
                key, val = parts[0], parts[1]
                if key.endswith('numCycles'):
                    try: stats['cycles'] = int(val)
                    except: pass
                elif key == 'simInsts' or key.endswith('committedInsts'):
                    try: stats['insts'] = int(val)
                    except: pass
                elif key.endswith('.ipc') or key == 'ipc':
                    try: stats['ipc'] = float(val)
                    except: pass
                elif key.endswith('icache.demandHits::total') or key.endswith('icache.demand_hits::total'):
                    try: stats['l1i_hits'] = int(val)
                    except: pass
                elif key.endswith('icache.demandAccesses::total') or key.endswith('icache.demand_accesses::total'):
                    try: stats['l1i_accesses'] = int(val)
                    except: pass
    return stats

def print_table(case_name, ts, g5):
    print("=" * 70)
    print(f"     Accuracy Delta Report: [{case_name}] vs gem5 Golden")
    print("=" * 70)
    print(f"{'Metric':<25} | {'TinyCpuSim':<14} | {'gem5 Golden':<14} | {'Delta (%)':<10}")
    print("-" * 70)
    
    metrics = [
        ('cycles', 'Simulated Cycles'),
        ('insts', 'Committed Insts'),
        ('ipc', 'Throughput (IPC)'),
    ]
    for key, label in metrics:
        v_ts = ts.get(key, 'N/A')
        v_g5 = g5.get(key, 'N/A')
        delta_str = 'N/A'
        if isinstance(v_ts, (int, float)) and isinstance(v_g5, (int, float)) and v_g5 > 0:
            delta = abs(v_ts - v_g5) / float(v_g5) * 100.0
            delta_str = f"{delta:.2f}%"
        
        print(f"{label:<25} | {str(v_ts):<14} | {str(v_g5):<14} | {delta_str:<10}")
    print("=" * 70)
    print()

import argparse
import concurrent.futures

def _run_single_case(args_tuple):
    g_file, fixtures_dir, sim_bin = args_tuple
    case_name = os.path.basename(g_file).replace('.stats.txt', '')
    elf_file = os.path.join(fixtures_dir, f"{case_name}.elf")
    if not os.path.exists(elf_file):
        return case_name, None, None, f"Warning: ELF fixture not found for {case_name}"
    res = subprocess.run([sim_bin, "--uarch", elf_file], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    ts = parse_tinysim_stats(res.stdout)
    g5 = parse_gem5_stats(g_file)
    return case_name, ts, g5, None

def main():
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    golden_dir = os.path.join(root_dir, "tests", "golden", "gem5")
    fixtures_dir = os.path.join(root_dir, "tests", "fixtures")
    sim_bin = os.path.join(root_dir, "build", "tinycpusim")
    if not os.path.exists(sim_bin):
        sim_bin = os.path.join(root_dir, "build", "tinyarmsim")

    default_jobs = max(1, (os.cpu_count() or 4) // 2)
    parser = argparse.ArgumentParser(description="TinyArmSim vs gem5 Golden Reference Accuracy Comparator")
    parser.add_argument("files", nargs="*", help="Optional <tinysim_stats.txt> <gem5_stats.txt> files")
    parser.add_argument("--all", "-a", action="store_true", help="Run batch comparison against all golden references")
    parser.add_argument("--case", help="Run comparison for a specific test case name")
    parser.add_argument("-j", "--jobs", type=int, default=default_jobs, help=f"Parallel worker threads/processes (default: {default_jobs}, half of CPU cores)")
    args = parser.parse_args()

    if args.all or (not args.case and not args.files):
        # Batch regression mode against all golden files
        golden_files = sorted(glob.glob(os.path.join(golden_dir, "*.stats.txt")))
        if not golden_files:
            print(f"No golden reference files found in {golden_dir}")
            sys.exit(1)
            
        print(f"Running automated regression comparison for {len(golden_files)} benchmarks...")
        print(f"Golden Directory: {golden_dir}")
        print(f"Parallel Workers: {args.jobs} (half of CPU cores)\n")
        
        task_args = [(g_file, fixtures_dir, sim_bin) for g_file in golden_files]
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
            results = list(executor.map(_run_single_case, task_args))

        for case_name, ts, g5, warn in results:
            if warn:
                print(warn)
                continue
            print_table(case_name, ts, g5)

    elif args.case:
        case_name = args.case
        g_file = os.path.join(golden_dir, f"{case_name}.stats.txt")
        elf_file = os.path.join(fixtures_dir, f"{case_name}.elf")
        res = subprocess.run([sim_bin, "--uarch", elf_file], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        ts = parse_tinysim_stats(res.stdout)
        g5 = parse_gem5_stats(g_file)
        print_table(case_name, ts, g5)

    elif len(args.files) >= 2:
        ts = parse_tinysim_stats(args.files[0])
        g5 = parse_gem5_stats(args.files[1])
        print_table(os.path.basename(args.files[0]), ts, g5)
    else:
        parser.print_help()

if __name__ == '__main__':
    main()
