#!/usr/bin/python3
"""
TinyCpuSim gem5 Golden Verification & Correlation Analysis Suite (scripts/verify_gem5.py)
Automates running all 9 benchmark ELFs against golden gem5 hardware performance statistics,
calculates metric correlation (Pearson r, Mean Absolute Percentage Error),
and verifies microarchitectural fidelity.
"""

import os
import sys
import re
import subprocess
import glob
import math

def find_default_tool(name):
    for candidate in [
        f"/usr/bin/{name}",
        f"/usr/local/bin/{name}",
        f"/opt/homebrew/bin/{name}",
        f"/bin/{name}"
    ]:
        if os.path.exists(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return name

CMAKE_BIN = find_default_tool("cmake")

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(PROJECT_ROOT, "build")
SIM_BIN = os.path.join(BUILD_DIR, "tinycpusim")
FIXTURES_DIR = os.path.join(PROJECT_ROOT, "tests", "fixtures")
GOLDEN_DIR = os.path.join(PROJECT_ROOT, "tests", "golden", "gem5")
DEFAULT_CFG = os.path.join(PROJECT_ROOT, "configs", "default", "default.cfg")

WORKLOADS = [
    "test_arithmetic.elf",
    "test_branch_pred.elf",
    "test_fibonacci.elf",
    "test_isa_coverage.elf",
    "test_mem_stride.elf",
    "test_raw_hazard.elf",
    "test_sort.elf",
    "test_store_forward.elf",
    "test_stress.elf"
]

def parse_gem5_stats(stats_path):
    if not os.path.exists(stats_path):
        return None
    stats = {}
    with open(stats_path, 'r') as fp:
        for line in fp:
            line = line.strip()
            if not line or line.startswith('#') or line.startswith('-'):
                continue
            parts = line.split('#')[0].split()
            if len(parts) >= 2:
                key = parts[0]
                val_str = parts[1]
                try:
                    if '.' in val_str:
                        stats[key] = float(val_str)
                    else:
                        stats[key] = int(val_str)
                except ValueError:
                    stats[key] = val_str

    extracted = {
        'insts': stats.get('simInsts', stats.get('sim_insts', 0)),
        'ops': stats.get('simOps', stats.get('sim_ops', 0)),
        'cycles': stats.get('system.cpu_cluster.cpus.numCycles', stats.get('system.cpu.numCycles', 0)),
        'ipc': stats.get('system.cpu_cluster.cpus.ipc', stats.get('system.cpu.ipc', 0.0)),
        'cond_predicted': stats.get('system.cpu_cluster.cpus.branchPred.condPredicted', 0),
        'cond_incorrect': stats.get('system.cpu_cluster.cpus.branchPred.condIncorrect', 0),
        'btb_hits': stats.get('system.cpu_cluster.cpus.branchPred.BTBHits', 0),
        'btb_lookups': stats.get('system.cpu_cluster.cpus.branchPred.BTBLookups', 0),
        'ras_used': stats.get('system.cpu_cluster.cpus.branchPred.RASUsed', 0),
        'ras_incorrect': stats.get('system.cpu_cluster.cpus.branchPred.RASIncorrect', 0),
        'dcache_hits': stats.get('system.cpu_cluster.cpus.dcache.overallHits::total', 0),
        'dcache_misses': stats.get('system.cpu_cluster.cpus.dcache.overallMisses::total', 0),
        'dcache_accesses': stats.get('system.cpu_cluster.cpus.dcache.overallAccesses::total', 0),
        'icache_hits': stats.get('system.cpu_cluster.cpus.icache.overallHits::total', 0),
        'icache_misses': stats.get('system.cpu_cluster.cpus.icache.overallMisses::total', 0),
        'icache_accesses': stats.get('system.cpu_cluster.cpus.icache.overallAccesses::total', 0),
        'l2_hits': stats.get('system.cpu_cluster.l2.overallHits::total', 0),
        'l2_misses': stats.get('system.cpu_cluster.l2.overallMisses::total', 0),
    }
    
    # Calculate rates
    if extracted['cond_predicted'] > 0:
        extracted['branch_acc'] = 100.0 * (1.0 - (float(extracted['cond_incorrect']) / float(extracted['cond_predicted'])))
    else:
        extracted['branch_acc'] = 100.0

    if extracted['dcache_accesses'] > 0:
        extracted['dcache_hit_rate'] = 100.0 * float(extracted['dcache_hits']) / float(extracted['dcache_accesses'])
    else:
        extracted['dcache_hit_rate'] = 100.0

    if extracted['icache_accesses'] > 0:
        extracted['icache_hit_rate'] = 100.0 * float(extracted['icache_hits']) / float(extracted['icache_accesses'])
    else:
        extracted['icache_hit_rate'] = 100.0

    return extracted

def parse_tinysim_stats(output_text):
    stats = {}
    m = re.search(r'Simulated Total Cycles:\s+(\d+)', output_text)
    if m: stats['cycles'] = int(m.group(1))

    m = re.search(r'Total Committed Insts:\s+(\d+)', output_text)
    if m: stats['insts'] = int(m.group(1))

    m = re.search(r'Total Committed uOps:\s+(\d+)', output_text)
    if m: stats['uops'] = int(m.group(1))

    m = re.search(r'Aggregate Throughput \(IPC\):\s*([\d\.]+)', output_text)
    if m: stats['ipc'] = float(m.group(1))

    m = re.search(r'Branch Predictions:\s+(\d+)\s+\(Accuracy:\s*([\d\.]+)%\)', output_text)
    if m:
        stats['branch_preds'] = int(m.group(1))
        stats['branch_acc'] = float(m.group(2))

    m = re.search(r'Branch Mispredicts:\s+(\d+)', output_text)
    if m: stats['branch_mispredicts'] = int(m.group(1))

    m = re.search(r'BTB Hits / Misses:\s+\d+\s+/\s+\d+\s+\(Hit Rate:\s*([\d\.]+)%\)', output_text)
    if m: stats['btb_hit_rate'] = float(m.group(1))

    m = re.search(r'RAS Hits / Misses:\s+\d+\s+/\s+\d+\s+\(Hit Rate:\s*([\d\.]+)%\)', output_text)
    if m: stats['ras_hit_rate'] = float(m.group(1))

    m = re.search(r'L1I Cache Hit Rate:\s*([\d\.]+)%', output_text)
    if m: stats['l1i_hit_rate'] = float(m.group(1))

    m = re.search(r'L1D Cache Hit Rate:\s*([\d\.]+)%', output_text)
    if m: stats['l1d_hit_rate'] = float(m.group(1))

    m = re.search(r'Shared L2 Cache Hit Rate:\s*([\d\.]+)%', output_text)
    if m: stats['l2_hit_rate'] = float(m.group(1))

    return stats

def run_tinysim(elf_path, cfg_path):
    cmd = [SIM_BIN, "--uarch", "--uarch-config", cfg_path, "--all-perf", elf_path]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return parse_tinysim_stats(res.stdout), res.stdout

def calculate_correlation(x_vals, y_vals):
    n = len(x_vals)
    if n < 2: return 1.0
    mean_x = sum(x_vals) / n
    mean_y = sum(y_vals) / n
    cov = sum((x - mean_x) * (y - mean_y) for x, y in zip(x_vals, y_vals))
    std_x = math.sqrt(sum((x - mean_x) ** 2 for x in x_vals))
    std_y = math.sqrt(sum((y - mean_y) ** 2 for y in y_vals))
    if std_x == 0 or std_y == 0: return 1.0
    return cov / (std_x * std_y)

import argparse
import concurrent.futures

def _eval_single_workload(args_tuple):
    elf_name, cfg_path = args_tuple
    elf_path = os.path.join(FIXTURES_DIR, elf_name)
    stats_name = elf_name.replace(".elf", ".stats.txt")
    golden_path = os.path.join(GOLDEN_DIR, stats_name)

    if not os.path.exists(elf_path):
        return elf_name, None, None, f"Warning: ELF fixture {elf_path} not found."
    if not os.path.exists(golden_path):
        return elf_name, None, None, f"Warning: gem5 golden file {golden_path} not found."

    gem5_stats = parse_gem5_stats(golden_path)
    tiny_stats, raw_log = run_tinysim(elf_path, cfg_path)
    return elf_name, gem5_stats, tiny_stats, None

def main():
    default_jobs = max(1, (os.cpu_count() or 4) // 2)
    parser = argparse.ArgumentParser(description="TinyCpuSim vs gem5 Golden Architectural Verification & Correlation")
    parser.add_argument("workloads", nargs="*", help="Optional specific ELF fixture names or paths to verify")
    parser.add_argument("-a", "--all", action="store_true", default=True, help="Verify against all golden reference benchmarks (default)")
    parser.add_argument("-j", "--jobs", type=int, default=default_jobs, help=f"Parallel worker threads/processes (default: {default_jobs}, half of CPU cores)")
    parser.add_argument("-c", "--config", default=DEFAULT_CFG, help=f"TinySim config file (default: {DEFAULT_CFG})")
    args = parser.parse_args()

    if not os.path.exists(SIM_BIN):
        print("Building simulator binary...")
        subprocess.run([CMAKE_BIN, "--build", BUILD_DIR, "-j", str(args.jobs)], check=True)

    selected_workloads = []
    if args.workloads:
        for w in args.workloads:
            base = os.path.basename(w)
            if not base.endswith(".elf"):
                base += ".elf"
            selected_workloads.append(base)
    else:
        selected_workloads = list(WORKLOADS)

    print("=" * 96)
    print("           TinyCpuSim vs gem5 Golden Architectural Verification & Correlation           ")
    print("=" * 96)
    print(f"  Golden Baselines Directory: {GOLDEN_DIR}")
    print(f"  Active Configuration:       {args.config}")
    print(f"  Parallel Workers:           {args.jobs} (half of CPU cores)")
    print("-" * 96)

    workload_args = [(elf_name, args.config) for elf_name in selected_workloads]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        eval_results = list(executor.map(_eval_single_workload, workload_args))

    table_rows = []
    gem5_insts_list = []
    tiny_insts_list = []
    gem5_cycles_list = []
    tiny_cycles_list = []
    gem5_ipc_list = []
    tiny_ipc_list = []

    for elf_name, gem5_stats, tiny_stats, warn_msg in eval_results:
        if warn_msg:
            print(warn_msg)
            continue

        g_insts = gem5_stats.get('insts', 0)
        t_insts = tiny_stats.get('insts', 0)
        inst_match = (g_insts == t_insts) or (abs(g_insts - t_insts) <= 2)

        g_cycles = gem5_stats.get('cycles', 0)
        t_cycles = tiny_stats.get('cycles', 0)

        g_ipc = gem5_stats.get('ipc', 0.0)
        t_ipc = tiny_stats.get('ipc', 0.0)

        g_bacc = gem5_stats.get('branch_acc', 100.0)
        t_bacc = tiny_stats.get('branch_acc', 100.0)

        g_l1d = gem5_stats.get('dcache_hit_rate', 100.0)
        t_l1d = tiny_stats.get('l1d_hit_rate', 100.0)

        gem5_insts_list.append(g_insts)
        tiny_insts_list.append(t_insts)
        gem5_cycles_list.append(g_cycles)
        tiny_cycles_list.append(t_cycles)
        gem5_ipc_list.append(g_ipc)
        tiny_ipc_list.append(t_ipc)

        table_rows.append({
            'elf': elf_name,
            'g_insts': g_insts,
            't_insts': t_insts,
            'inst_match': inst_match,
            'g_cycles': g_cycles,
            't_cycles': t_cycles,
            'g_ipc': g_ipc,
            't_ipc': t_ipc,
            'g_bacc': g_bacc,
            't_bacc': t_bacc,
            'g_l1d': g_l1d,
            't_l1d': t_l1d
        })

    # Header
    print(f"{'Workload ELF':<24} | {'gem5 Inst':<10} | {'Tiny Inst':<10} | {'Inst Δ%':<8} | {'gem5 IPC':<9} | {'Tiny IPC':<9} | {'IPC Δ%':<8} | {'Status':<8}")
    print("-" * 105)
    
    pass_count = 0
    total_count = len(table_rows)
    inst_deltas = []
    ipc_deltas = []
    max_ipc_discrepancy = -1.0
    max_ipc_case = None

    for r in table_rows:
        inst_delta = abs(r['t_insts'] - r['g_insts']) / float(r['g_insts']) * 100.0 if r['g_insts'] > 0 else 0.0
        ipc_delta = abs(r['t_ipc'] - r['g_ipc']) / float(r['g_ipc']) * 100.0 if r['g_ipc'] > 0 else 0.0
        
        inst_deltas.append(inst_delta)
        ipc_deltas.append(ipc_delta)
        
        if ipc_delta > max_ipc_discrepancy:
            max_ipc_discrepancy = ipc_delta
            max_ipc_case = r

        status_pass = (inst_delta <= 5.0) and (ipc_delta <= 5.0)
        if status_pass:
            pass_count += 1
            status_tag = "\033[1;32mPASS\033[0m"
        else:
            status_tag = "\033[1;31mFAIL\033[0m"
        
        print(f"{r['elf']:<24} | {r['g_insts']:<10} | {r['t_insts']:<10} | {inst_delta:<7.2f}% | {r['g_ipc']:<9.3f} | {r['t_ipc']:<9.3f} | {ipc_delta:<7.2f}% | {status_tag:<8}")

    # Statistical Correlation
    r_insts = calculate_correlation(gem5_insts_list, tiny_insts_list)
    r_cycles = calculate_correlation(gem5_cycles_list, tiny_cycles_list)
    r_ipc = calculate_correlation(gem5_ipc_list, tiny_ipc_list)

    inst_mape = sum(inst_deltas) / float(total_count) if total_count > 0 else 0.0
    ipc_mape = sum(ipc_deltas) / float(total_count) if total_count > 0 else 0.0
    pass_rate_pct = (float(pass_count) / float(total_count) * 100.0) if total_count > 0 else 0.0

    print("=" * 105)
    print("  📈 CORRELATION & FIDELITY SUMMARY vs gem5 GOLDEN:")
    print("=" * 105)
    pass_color = "\033[1;32m" if pass_count == total_count else "\033[1;33m"
    print(f"  • Passing Workloads (Δ <= 5.0% Inst & IPC):   {pass_color}{pass_count}/{total_count} ({pass_rate_pct:.1f}%)\033[0m")
    print(f"  • Mean Absolute Percentage Error (MAPE):      Inst: \033[1;32m{inst_mape:.2f}%\033[0m | IPC: \033[1;32m{ipc_mape:.2f}%\033[0m")
    print(f"  • Instruction Count Pearson Correlation r:    \033[1;32m{r_insts:.4f}\033[0m")
    print(f"  • Simulated Cycles Pearson Correlation r:     \033[1;32m{r_cycles:.4f}\033[0m")
    print(f"  • IPC Pearson Correlation r:                  \033[1;32m{r_ipc:.4f}\033[0m")
    if max_ipc_case:
        print(f"  • Maximum IPC Discrepancy ({max_ipc_case['elf']}): \033[1;33m{max_ipc_discrepancy:.2f}% (TinySim: {max_ipc_case['t_ipc']:.3f} vs gem5: {max_ipc_case['g_ipc']:.3f})\033[0m")
    print("=" * 105)

if __name__ == '__main__':
    main()
