#!/usr/bin/python3
"""
TinyCpuSim Microarchitecture Parameter Sweep Utility
Runs parameter sweeps (e.g. ROB Size, Issue Width, L1 Cache Size, Branch Predictor)
across target bare-metal ELFs or microbenchmarks, outputting an ANSI comparison table.
"""

import os
import sys
import argparse
import subprocess
import re
import tempfile

def parse_stats(output_text):
    stats = {}
    m = re.search(r'Simulated Total Cycles:\s+(\d+)', output_text)
    if m: stats['cycles'] = int(m.group(1))
    
    m = re.search(r'Total Committed Insts:\s+(\d+)', output_text)
    if m: stats['insts'] = int(m.group(1))

    m = re.search(r'Aggregate Throughput \(IPC\):\s*([\d\.]+)', output_text)
    if m: stats['ipc'] = float(m.group(1))

    m = re.search(r'Branch Predictions:\s+\d+\s+\(Accuracy:\s*([\d\.]+)%\)', output_text)
    if m: stats['branch_acc'] = float(m.group(1))

    m = re.search(r'L1D Cache Hit Rate:\s*([\d\.]+)%', output_text)
    if m: stats['l1d_hit_rate'] = float(m.group(1))
    return stats

def generate_temp_config(base_cfg_path, overrides):
    content = ""
    if os.path.exists(base_cfg_path):
        with open(base_cfg_path, 'r') as f:
            content = f.read()
    else:
        # Default config skeleton
        content = """[core]
fetch_width = 4
decode_width = 4
issue_width = 4
commit_width = 4
rob_size = 64
issue_queue_size = 32
num_phys_registers = 96
num_load_queue_entries = 16
num_store_queue_entries = 16

[branch_predictor]
enabled = true
type = TAGE
table_size = 2048
btb_size = 1024
ras_size = 16

[cache_l1i]
enabled = true
size = 32768
associativity = 4
line_size = 64
hit_latency = 1

[cache_l1d]
enabled = true
size = 32768
associativity = 4
line_size = 64
hit_latency = 1
"""

    lines = content.splitlines()
    for section, key, val in overrides:
        # Simple override or append
        found = False
        new_lines = []
        in_section = False
        for line in lines:
            if line.strip().startswith('[') and line.strip().endswith(']'):
                curr_sec = line.strip()[1:-1]
                in_section = (curr_sec == section)
            if in_section and line.strip().startswith(f"{key} =") or (in_section and line.strip().startswith(f"{key}=")):
                new_lines.append(f"{key} = {val}")
                found = True
            else:
                new_lines.append(line)
        if not found:
            new_lines.append(f"\n[{section}]\n{key} = {val}")
        lines = new_lines

    tmp = tempfile.NamedTemporaryFile(mode='w', suffix='.cfg', delete=False)
    tmp.write("\n".join(lines))
    tmp.close()
    return tmp.name

def run_simulation(sim_bin, elf_path, cfg_path):
    cmd = [sim_bin, "--uarch", "--uarch-config", cfg_path, "--all-perf", elf_path]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return parse_stats(res.stdout)

import concurrent.futures

def _run_sweep_item(args_tuple):
    label, overrides, base_cfg, sim_bin, elf_file = args_tuple
    tmp_cfg = generate_temp_config(base_cfg, overrides)
    try:
        stats = run_simulation(sim_bin, elf_file, tmp_cfg)
        return label, stats
    finally:
        if os.path.exists(tmp_cfg):
            try:
                os.remove(tmp_cfg)
            except OSError:
                pass

def main():
    default_jobs = max(1, (os.cpu_count() or 4) // 2)
    parser = argparse.ArgumentParser(description="TinyCpuSim Parameter Sweep Utility")
    parser.add_argument("--elf", default="tests/fixtures/test_fibonacci.elf", help="Target ELF binary")
    parser.add_argument("--param", choices=["rob_size", "issue_width", "l1d_size", "bp_type"], default="rob_size", help="Parameter to sweep")
    parser.add_argument("--base-cfg", default="configs/default/default.cfg", help="Base config file")
    parser.add_argument("-j", "--jobs", type=int, default=default_jobs, help=f"Parallel worker threads/processes (default: {default_jobs}, half of CPU cores)")
    args = parser.parse_args()

    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sim_bin = os.path.join(root_dir, "build", "tinycpusim")
    elf_file = os.path.join(root_dir, args.elf) if not os.path.isabs(args.elf) else args.elf
    base_cfg = os.path.join(root_dir, args.base_cfg) if not os.path.isabs(args.base_cfg) else args.base_cfg

    if not os.path.exists(sim_bin):
        print(f"Error: Simulator binary {sim_bin} not found. Run scripts/01_build.sh first.")
        sys.exit(1)

    if not os.path.exists(elf_file):
        print(f"Error: ELF file {elf_file} not found.")
        sys.exit(1)

    sweep_configs = []
    if args.param == "rob_size":
        values = [16, 32, 64, 128, 256]
        sweep_configs = [(f"ROB={v}", [("core", "rob_size", str(v))]) for v in values]
    elif args.param == "issue_width":
        values = [1, 2, 4, 8]
        sweep_configs = [(f"Width={v}-way", [
            ("core", "fetch_width", str(v)),
            ("core", "decode_width", str(v)),
            ("core", "issue_width", str(v)),
            ("core", "commit_width", str(v))
        ]) for v in values]
    elif args.param == "l1d_size":
        values = [8192, 16384, 32768, 65536]
        sweep_configs = [(f"L1D={v//1024}KB", [("cache_l1d", "size", str(v))]) for v in values]
    elif args.param == "bp_type":
        values = ["BIMODAL", "GSHARE", "TAGE"]
        sweep_configs = [(f"BP={v}", [("branch_predictor", "type", v)]) for v in values]

    print("=" * 75)
    print(f"  TinyCpuSim Parameter Sweep: [{args.param}] on {os.path.basename(elf_file)}")
    print(f"  Base Config:      {os.path.basename(base_cfg)}")
    print(f"  Parallel Workers: {args.jobs} (half of CPU cores)")
    print("=" * 75)
    print(f"{'Configuration':<18} | {'Cycles':<10} | {'IPC':<8} | {'Branch Acc':<12} | {'L1D Hit Rate':<12}")
    print("-" * 75)

    task_args = [(label, overrides, base_cfg, sim_bin, elf_file) for label, overrides in sweep_configs]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        results = list(executor.map(_run_sweep_item, task_args))

    for label, stats in results:
        cycles = stats.get('cycles', 'N/A')
        ipc = f"{stats.get('ipc', 0.0):.3f}" if 'ipc' in stats else 'N/A'
        b_acc = f"{stats.get('branch_acc', 0.0):.1f}%" if 'branch_acc' in stats else 'N/A'
        l1d = f"{stats.get('l1d_hit_rate', 0.0):.1f}%" if 'l1d_hit_rate' in stats else 'N/A'
        print(f"{label:<18} | {str(cycles):<10} | {ipc:<8} | {b_acc:<12} | {l1d:<12}")

    print("=" * 75)

if __name__ == '__main__':
    main()
