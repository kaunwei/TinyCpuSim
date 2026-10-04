#!/usr/bin/python3
"""
TinyCpuSim vs gem5 Detailed Stage-by-Stage Performance Counter Comparator (scripts/compare_perf_counters.py)
Compares fine-grained performance counters between gem5 stats.txt and TinyCpuSim uArch report
to isolate drops, stalls, and pipeline bottlenecks stage-by-stage.
"""

import os
import sys
import re
import argparse
import subprocess

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(PROJECT_ROOT, "build")
SIM_BIN = os.path.join(BUILD_DIR, "tinycpusim")
GOLDEN_DIR = os.path.join(PROJECT_ROOT, "tests", "golden", "gem5")
DEFAULT_CFG = os.path.join(PROJECT_ROOT, "configs", "default", "gem5_o3.cfg")

def parse_gem5_stats(stats_path):
    if not os.path.exists(stats_path):
        raise FileNotFoundError(f"gem5 stats file not found: {stats_path}")
    raw = {}
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
                        raw[key] = float(val_str)
                    else:
                        raw[key] = int(val_str)
                except ValueError:
                    raw[key] = val_str

    extracted = {
        'insts': raw.get('simInsts', raw.get('sim_insts', 0)),
        'ops': raw.get('simOps', raw.get('sim_ops', 0)),
        'total_cycles': raw.get('system.cpu_cluster.cpus.numCycles', raw.get('system.cpu.numCycles', 0)),
        'idle_cycles': raw.get('system.cpu_cluster.cpus.idleCycles', 0),
        'ipc': raw.get('system.cpu_cluster.cpus.ipc', 0.0),
        'running_cycles': raw.get('system.cpu_cluster.cpus.commit.status::running', 0),
        'squashing_cycles': raw.get('system.cpu_cluster.cpus.commit.status::robSquashing', 0),
        
        # Branch Predictor
        'bp_lookups': raw.get('system.cpu_cluster.cpus.branchPred.lookups_0::total', 0),
        'bp_direct_cond': raw.get('system.cpu_cluster.cpus.branchPred.lookups_0::DirectCond', 0),
        'bp_direct_uncond': raw.get('system.cpu_cluster.cpus.branchPred.lookups_0::DirectUncond', 0),
        'bp_calls': raw.get('system.cpu_cluster.cpus.branchPred.lookups_0::CallDirect', 0) + raw.get('system.cpu_cluster.cpus.branchPred.lookups_0::CallIndirect', 0),
        'bp_returns': raw.get('system.cpu_cluster.cpus.branchPred.lookups_0::Return', 0),
        'bp_squashes': raw.get('system.cpu_cluster.cpus.branchPred.squashes_0::total', 0),
        'bp_committed_mispredicts': raw.get('system.cpu_cluster.cpus.branchPred.mispredicted_0::total', 0),
        'bp_mispredict_dir': raw.get('system.cpu_cluster.cpus.branchPred.mispredictDueToPredictor_0::total', raw.get('system.cpu_cluster.cpus.branchPred.mispredictDueToPredictor_0::DirectCond', 0)),
        'bp_mispredict_btb': raw.get('system.cpu_cluster.cpus.branchPred.mispredictDueToBTBMiss_0::total', 0),
        'btb_hits': raw.get('system.cpu_cluster.cpus.branchPred.BTBHits', 0),
        'btb_lookups': raw.get('system.cpu_cluster.cpus.branchPred.BTBLookups', 0),
        'btb_misses': raw.get('system.cpu_cluster.cpus.branchPred.btb.misses::total', 0),
        'ras_used': raw.get('system.cpu_cluster.cpus.branchPred.ras.used', 0),
        'ras_correct': raw.get('system.cpu_cluster.cpus.branchPred.ras.correct', 0),
        
        # Cache Counters
        'icache_accesses': raw.get('system.cpu_cluster.cpus.icache.overallAccesses::total', 0),
        'icache_hits': raw.get('system.cpu_cluster.cpus.icache.overallHits::total', 0),
        'icache_misses': raw.get('system.cpu_cluster.cpus.icache.overallMisses::total', 0),
        'dcache_accesses': raw.get('system.cpu_cluster.cpus.dcache.overallAccesses::total', 0),
        'dcache_hits': raw.get('system.cpu_cluster.cpus.dcache.overallHits::total', 0),
        'dcache_misses': raw.get('system.cpu_cluster.cpus.dcache.overallMisses::total', 0),
        'l2_accesses': raw.get('system.cpu_cluster.l2.overallAccesses::total', 0),
        'l2_hits': raw.get('system.cpu_cluster.l2.overallHits::total', 0),
        'l2_misses': raw.get('system.cpu_cluster.l2.overallMisses::total', 0),
        
        # LSU / Execute
        'issued_insts': raw.get('system.cpu_cluster.cpus.instsIssued', 0),
        'squashed_insts': raw.get('system.cpu_cluster.cpus.numSquashedInsts', 0),
        'int_alu_accesses': raw.get('system.cpu_cluster.cpus.intAluAccesses', 0),
    }

    if extracted['running_cycles'] == 0:
        extracted['running_cycles'] = extracted['total_cycles'] - extracted['idle_cycles']
    extracted['active_ipc'] = (float(extracted['insts']) / float(extracted['running_cycles'])) if extracted['running_cycles'] > 0 else 0.0

    return extracted

def parse_tinysim_stats(output_text):
    stats = {}
    m = re.search(r'Simulated Total Cycles:\s+(\d+)', output_text)
    if m: stats['total_cycles'] = int(m.group(1))

    m = re.search(r'Total Committed Insts:\s+(\d+)', output_text)
    if m: stats['insts'] = int(m.group(1))

    m = re.search(r'Total Committed uOps:\s+(\d+)', output_text)
    if m: stats['ops'] = int(m.group(1))

    m = re.search(r'Aggregate Throughput \(IPC\):\s*([\d\.]+)', output_text)
    if m: stats['ipc'] = float(m.group(1))

    # Branch stats
    m = re.search(r'Branch Predictions:\s+(\d+)', output_text)
    if m: stats['bp_lookups'] = int(m.group(1))

    m = re.search(r'Branch Mispredicts:\s+(\d+)', output_text)
    if m: stats['bp_committed_mispredicts'] = int(m.group(1))

    m = re.search(r'Direct Cond / Uncond:\s+(\d+)\s+/\s+(\d+)', output_text)
    if m:
        stats['bp_direct_cond'] = int(m.group(1))
        stats['bp_direct_uncond'] = int(m.group(2))

    m = re.search(r'Calls / Returns:\s+(\d+)\s+/\s+(\d+)', output_text)
    if m:
        stats['bp_calls'] = int(m.group(1))
        stats['bp_returns'] = int(m.group(2))

    m = re.search(r'Mispredict Dir / BTB:\s+(\d+)\s+/\s+(\d+)', output_text)
    if m:
        stats['bp_mispredict_dir'] = int(m.group(1))
        stats['bp_mispredict_btb'] = int(m.group(2))

    m = re.search(r'Squashed Spec Branches:\s*(\d+)', output_text)
    if m:
        stats['bp_squashes'] = int(m.group(1))

    m = re.search(r'BTB Hits / Misses:\s+(\d+)\s+/\s+(\d+)', output_text)
    if m:
        stats['btb_hits'] = int(m.group(1))
        stats['btb_misses'] = int(m.group(2))

    m = re.search(r'RAS Pushes / Pops:\s+(\d+)\s+/\s+(\d+)', output_text)
    if m:
        stats['ras_pushes'] = int(m.group(1))
        stats['ras_used'] = int(m.group(2))

    # Caches
    m = re.search(r'L1I Cache Hit Rate:\s*[\d\.]+% \((\d+)/(\d+)\)', output_text)
    if m:
        stats['icache_hits'] = int(m.group(1))
        stats['icache_accesses'] = int(m.group(2))
        stats['icache_misses'] = stats['icache_accesses'] - stats['icache_hits']

    m = re.search(r'L1D Cache Hit Rate:\s*[\d\.]+% \((\d+)/(\d+)\)', output_text)
    if m:
        stats['dcache_hits'] = int(m.group(1))
        stats['dcache_accesses'] = int(m.group(2))
        stats['dcache_misses'] = stats['dcache_accesses'] - stats['dcache_hits']

    m = re.search(r'L2 Accesses:\s+(\d+)', output_text)
    if m:
        stats['l2_accesses'] = int(m.group(1))
    m = re.search(r'L2 Hits / Misses:\s+(\d+)\s+/\s+(\d+)', output_text)
    if m:
        stats['l2_hits'] = int(m.group(1))
        stats['l2_misses'] = int(m.group(2))

    # Ports
    m = re.search(r'Port ALU uOps:\s+(\d+)', output_text)
    if m: stats['int_alu_accesses'] = int(m.group(1))

    m = re.search(r'Port Branch uOps:\s+(\d+)', output_text)
    if m: stats['port_branch'] = int(m.group(1))

    m = re.search(r'Port LSU uOps:\s+(\d+)', output_text)
    if m: stats['port_lsu'] = int(m.group(1))

    return stats

def run_tinysim(elf_path, cfg_path):
    cmd = [SIM_BIN, "--uarch", "--uarch-config", cfg_path, "--all-perf", elf_path]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return parse_tinysim_stats(res.stdout), res.stdout

def print_diff_table(title, rows):
    print(f"\n[{title}]")
    print(f"{'Performance Metric':<32} | {'gem5 Golden':<14} | {'TinySim':<14} | {'Delta (%)':<12} | {'Fidelity'}")
    print("-" * 88)
    for label, g_val, t_val, unit, lower_better in rows:
        if isinstance(g_val, (int, float)) and isinstance(t_val, (int, float)):
            if g_val == 0 and t_val == 0:
                delta_str = "0.0%"
                fidelity = "\033[1;32mPERFECT\033[0m"
            elif g_val == 0:
                delta_str = f"+{t_val:.1f}"
                fidelity = "\033[1;33mDIVERGE\033[0m"
            else:
                pct = ((float(t_val) - float(g_val)) / float(g_val)) * 100.0
                delta_str = f"{pct:+.2f}%"
                if abs(pct) <= 5.0:
                    fidelity = "\033[1;32mMATCH (<5%)\033[0m"
                elif abs(pct) <= 15.0:
                    fidelity = "\033[1;33mCLOSE (<15%)\033[0m"
                else:
                    fidelity = "\033[1;31mDROP/DELTA\033[0m"
            
            g_str = f"{g_val:.3f}" if isinstance(g_val, float) else f"{g_val}"
            t_str = f"{t_val:.3f}" if isinstance(t_val, float) else f"{t_val}"
            print(f"{label:<32} | {g_str:<14} | {t_str:<14} | {delta_str:<12} | {fidelity}")
        else:
            print(f"{label:<32} | {str(g_val):<14} | {str(t_val):<14} | {'---':<12} | ---")

def compare_single_workload(elf_name, cfg_path):
    elf_path = os.path.join(PROJECT_ROOT, "tests", "fixtures", elf_name)
    golden_path = os.path.join(GOLDEN_DIR, elf_name.replace(".elf", ".stats.txt"))

    if not os.path.exists(elf_path):
        print(f"Error: ELF fixture not found: {elf_path}")
        return
    if not os.path.exists(golden_path):
        print(f"Error: gem5 golden file not found: {golden_path}")
        return

    g = parse_gem5_stats(golden_path)
    t, raw_out = run_tinysim(elf_path, cfg_path)

    print("=" * 88)
    print(f"   Stage-by-Stage Performance Counter Audit: {elf_name}")
    print(f"   Config: {cfg_path}")
    print("=" * 88)

    # 1. Architectural & Timing Overview
    print_diff_table("1. Architectural & High-Level Timing", [
        ("Committed Instructions", g.get('insts', 0), t.get('insts', 0), "", False),
        ("Committed Ops / uOps", g.get('ops', 0), t.get('ops', 0), "", False),
        ("Active Core Cycles", g.get('running_cycles', 0), t.get('total_cycles', 0), "", True),
        ("Throughput IPC", g.get('ipc', 0.0), t.get('ipc', 0.0), "", False),
    ])

    # 2. Frontend & Branch Predictor
    print_diff_table("2. Frontend & Branch Predictor Diagnostics", [
        ("Branch Predictions (Lookups)", g.get('bp_lookups', 0), t.get('bp_lookups', 0), "", False),
        ("Direct Conditional Branches", g.get('bp_direct_cond', 0), t.get('bp_direct_cond', 0), "", False),
        ("Direct Unconditional Branches", g.get('bp_direct_uncond', 0), t.get('bp_direct_uncond', 0), "", False),
        ("Function Calls (BL/BLX)", g.get('bp_calls', 0), t.get('bp_calls', 0), "", False),
        ("Function Returns (BX LR/POP PC)", g.get('bp_returns', 0), t.get('bp_returns', 0), "", False),
        ("Committed Branch Mispredicts", g.get('bp_committed_mispredicts', 0), t.get('bp_committed_mispredicts', 0), "", True),
        ("Mispredict Due to Direction", g.get('bp_mispredict_dir', 0), t.get('bp_mispredict_dir', 0), "", True),
        ("Mispredict Due to BTB Miss", g.get('bp_mispredict_btb', 0), t.get('bp_mispredict_btb', 0), "", True),
        ("Squashed Spec Branches", g.get('bp_squashes', 0), t.get('bp_squashes', 0), "", True),
        ("BTB Hits", g.get('btb_hits', 0), t.get('btb_hits', 0), "", False),
        ("BTB Misses", g.get('btb_misses', 0), t.get('btb_misses', 0), "", True),
        ("RAS Returns Used", g.get('ras_used', 0), t.get('ras_used', 0), "", False),
    ])

    # 3. Cache Subsystem
    print_diff_table("3. Cache Subsystem & Memory Hierarchy", [
        ("L1I Cache Accesses", g.get('icache_accesses', 0), t.get('icache_accesses', 0), "", False),
        ("L1I Cache Hits", g.get('icache_hits', 0), t.get('icache_hits', 0), "", False),
        ("L1I Cache Misses", g.get('icache_misses', 0), t.get('icache_misses', 0), "", True),
        ("L1D Cache Accesses", g.get('dcache_accesses', 0), t.get('dcache_accesses', 0), "", False),
        ("L1D Cache Hits", g.get('dcache_hits', 0), t.get('dcache_hits', 0), "", False),
        ("L1D Cache Misses", g.get('dcache_misses', 0), t.get('dcache_misses', 0), "", True),
        ("L2 Cache Accesses", g.get('l2_accesses', 0), t.get('l2_accesses', 0), "", False),
        ("L2 Cache Misses", g.get('l2_misses', 0), t.get('l2_misses', 0), "", True),
    ])

    # 4. Execution Engine
    print_diff_table("4. Execution Engine & Port Scheduling", [
        ("Integer ALU Accesses", g.get('int_alu_accesses', 0), t.get('int_alu_accesses', 0), "", False),
    ])
    print("=" * 88)

def main():
    parser = argparse.ArgumentParser(description="Detailed Stage-by-Stage Performance Counter Comparator")
    parser.add_argument("elf", nargs="?", default="test_branch_pred.elf", help="Target ELF benchmark")
    parser.add_argument("--config", default=DEFAULT_CFG, help="TinySim configuration file")
    args = parser.parse_args()

    if not os.path.exists(SIM_BIN):
        subprocess.run(["cmake", "--build", BUILD_DIR, "-j4"], check=True)

    compare_single_workload(args.elf, args.config)

if __name__ == "__main__":
    main()
