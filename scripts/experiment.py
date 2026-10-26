#!/usr/bin/python3
"""
TinyCpuSim Microarchitectural Experiment & Parameter Exploration Utility
Allows tuning any hardware parameter on single or multiple ELF programs,
listing all tunable knobs, and generating automated side-by-side delta comparisons.
"""

import os
import sys
import argparse
import subprocess
import re
import tempfile
import glob
import configparser

if __package__:
    from .byte_sizes import parse_byte_size as _parse_byte_size
else:
    from byte_sizes import parse_byte_size as _parse_byte_size

# -----------------------------------------------------------------------------
# Microarchitectural Parameters Catalog
# -----------------------------------------------------------------------------
PARAM_CATALOG = {
    "core": {
        "description": "Superscalar Out-of-Order Pipeline & Execution Core",
        "params": {
            "enable_ooo": {"default": "true", "type": "bool", "desc": "Out-of-Order engine (true=Dynamic RS/PRF/ROB, false=In-Order)"},
            "fetch_width": {"default": 4, "type": "int", "desc": "Instruction fetch width per cycle"},
            "decode_width": {"default": 4, "type": "int", "desc": "Instruction decode width per cycle"},
            "rename_width": {"default": 4, "type": "int", "desc": "Register renaming width per cycle"},
            "issue_width": {"default": 4, "type": "int", "desc": "Superscalar issue / dispatch width per cycle"},
            "commit_width": {"default": 4, "type": "int", "desc": "In-order retirement width per cycle"},
            "rob_size": {"default": 64, "type": "int", "desc": "Reorder Buffer (ROB) capacity entries"},
            "rs_size": {"default": 32, "type": "int", "desc": "Issue Queue / Reservation Station capacity"},
            "num_phys_regs": {"default": 128, "type": "int", "desc": "Physical Register File (PRF) total registers"},
        }
    },
    "branch_predictor": {
        "description": "Branch Prediction Unit (BPU)",
        "params": {
            "enabled": {"default": "true", "type": "bool", "desc": "Enable or disable dynamic branch prediction"},
            "type": {"default": "TAGE", "type": "choice [IDEAL, BIMODAL, GSHARE, TAGE]", "desc": "Branch direction predictor algorithm"},
            "table_size": {"default": 4096, "type": "int (power of 2)", "desc": "Branch history table (PHT) entries"},
            "btb_size": {"default": 4096, "type": "int (power of 2)", "desc": "Branch Target Buffer (BTB) cache entries"},
            "ras_size": {"default": 32, "type": "int", "desc": "Return Address Stack (RAS) depth"},
            "tage_tables": {"default": 4, "type": "int", "desc": "Number of geometric history TAGE tables"},
        }
    },
    "lsu": {
        "description": "Load/Store Unit & Memory Disambiguation",
        "params": {
            "lq_size": {"default": 16, "type": "int", "desc": "Load Queue capacity entries"},
            "sq_size": {"default": 16, "type": "int", "desc": "Store Queue capacity entries"},
            "enable_store_forwarding": {"default": "true", "type": "bool", "desc": "Enable 0-cycle Store-to-Load Queue forwarding"},
            "enable_speculative_load": {"default": "true", "type": "bool", "desc": "Enable speculative load execution before prior store addresses resolve"},
            "store_forward_latency": {"default": 1, "type": "int", "desc": "Forwarding bypass latency in cycles"},
        }
    },
    "cache_l1i": {
        "description": "L1 Instruction Cache",
        "params": {
            "enabled": {"default": "true", "type": "bool", "desc": "Enable L1 Instruction cache"},
            "size_bytes": {"default": 32768, "type": "bytes (e.g. 16KB, 32KB, 64KB)", "desc": "L1I total capacity in bytes"},
            "associativity": {"default": 4, "type": "int (e.g. 2, 4, 8)", "desc": "L1I N-way set associativity"},
            "line_size": {"default": 64, "type": "bytes", "desc": "L1I cache block size in bytes"},
            "hit_latency_cycles": {"default": 1, "type": "int", "desc": "L1I hit latency in clock cycles"},
            "mshr_entries": {"default": 8, "type": "int", "desc": "L1I non-blocking Miss Status Holding Registers"},
        }
    },
    "cache_l1d": {
        "description": "L1 Data Cache",
        "params": {
            "enabled": {"default": "true", "type": "bool", "desc": "Enable L1 Data cache"},
            "size_bytes": {"default": 32768, "type": "bytes (e.g. 16KB, 32KB, 64KB)", "desc": "L1D total capacity in bytes"},
            "associativity": {"default": 4, "type": "int (e.g. 2, 4, 8)", "desc": "L1D N-way set associativity"},
            "line_size": {"default": 64, "type": "bytes", "desc": "L1D cache block size in bytes"},
            "hit_latency_cycles": {"default": 1, "type": "int", "desc": "L1D hit latency in clock cycles"},
            "mshr_entries": {"default": 8, "type": "int", "desc": "L1D non-blocking Miss Status Holding Registers"},
        }
    },
    "cache_l2": {
        "description": "Shared L2 Cache Subsystem",
        "params": {
            "enabled": {"default": "true", "type": "bool", "desc": "Enable shared L2 cache"},
            "size_bytes": {"default": 524288, "type": "bytes (e.g. 256KB, 512KB, 1MB)", "desc": "Shared L2 total capacity in bytes"},
            "associativity": {"default": 8, "type": "int (e.g. 4, 8, 16)", "desc": "Shared L2 N-way set associativity"},
            "hit_latency_cycles": {"default": 10, "type": "int", "desc": "L2 hit latency in clock cycles"},
            "mshr_entries": {"default": 16, "type": "int", "desc": "L2 non-blocking Miss Status Holding Registers"},
        }
    },
    "system": {
        "description": "System Bus & Main Memory",
        "params": {
            "dram_latency_cycles": {"default": 80, "type": "int", "desc": "Off-chip DRAM access penalty in cycles"},
            "enable_mesi_coherence": {"default": "true", "type": "bool", "desc": "Enable multi-core MESI snooping protocol"},
        }
    }
}

ELF_CATALOG = {
    "test_fibonacci.elf": "Recursive & iterative Fibonacci calculation (heavy branching, deep call stack, ALU)",
    "test_sort.elf": "In-place Bubble Sort (data-dependent conditional branches, sequential memory writes)",
    "test_stress.elf": "10,000 iteration computational stress loop (maximum superscalar issue saturation)",
    "test_mem_stride.elf": "Non-contiguous memory access stride pattern (cache misses, spatial locality stress)",
    "test_store_forward.elf": "Immediate store-then-load to identical address (LSQ store-to-load forwarding bypass)",
    "test_raw_hazard.elf": "Chained RAW data dependencies (instruction serialization & pipeline dependency latency)",
    "test_branch_pred.elf": "Mixed pattern & alternating branches (BPU saturating counter & TAGE warmup)",
    "test_arithmetic.elf": "Pure arithmetic and logical instructions (straight-line multi-ALU throughput)",
    "test_isa_coverage.elf": "Broad ARMv7-M Thumb-2 instruction coverage (IT blocks, shifts, 32-bit ops)",
}

# Parameter Aliases for Quick CLI Use
PARAM_ALIASES = {
    "ooo": ("core", "enable_ooo"),
    "enable_ooo": ("core", "enable_ooo"),
    "in_order": ("core", "enable_ooo"),
    "width": ("core", "issue_width"),
    "fetch_width": ("core", "fetch_width"),
    "decode_width": ("core", "decode_width"),
    "issue_width": ("core", "issue_width"),
    "commit_width": ("core", "commit_width"),
    "rob": ("core", "rob_size"),
    "rob_size": ("core", "rob_size"),
    "iq": ("core", "rs_size"),
    "iq_size": ("core", "rs_size"),
    "rs": ("core", "rs_size"),
    "rs_size": ("core", "rs_size"),
    "prf": ("core", "num_phys_regs"),
    "num_phys_regs": ("core", "num_phys_regs"),
    "bp": ("branch_predictor", "type"),
    "bp_type": ("branch_predictor", "type"),
    "bpu": ("branch_predictor", "type"),
    "bp_enabled": ("branch_predictor", "enabled"),
    "branch_enabled": ("branch_predictor", "enabled"),
    "bpu_enabled": ("branch_predictor", "enabled"),
    "btb": ("branch_predictor", "btb_size"),
    "btb_size": ("branch_predictor", "btb_size"),
    "ras": ("branch_predictor", "ras_size"),
    "ras_size": ("branch_predictor", "ras_size"),
    "lq": ("lsu", "lq_size"),
    "lq_size": ("lsu", "lq_size"),
    "sq": ("lsu", "sq_size"),
    "sq_size": ("lsu", "sq_size"),
    "store_forward": ("lsu", "enable_store_forwarding"),
    "l1i_size": ("cache_l1i", "size_bytes"),
    "l1d_size": ("cache_l1d", "size_bytes"),
    "l1i_assoc": ("cache_l1i", "associativity"),
    "l1d_assoc": ("cache_l1d", "associativity"),
    "l1d_latency": ("cache_l1d", "hit_latency_cycles"),
    "l2_size": ("cache_l2", "size_bytes"),
    "l2_assoc": ("cache_l2", "associativity"),
    "l2_latency": ("cache_l2", "hit_latency_cycles"),
    "dram_latency": ("system", "dram_latency_cycles"),
}

def parse_byte_size(val_str):
    return _parse_byte_size(val_str)

def list_parameters():
    print("=" * 80)
    print("      TinyCpuSim Tunable Microarchitectural Hardware Parameters Catalog      ")
    print("=" * 80)
    for sec_name, sec_data in PARAM_CATALOG.items():
        print(f"\n[{sec_name}] - {sec_data['description']}")
        print("-" * 80)
        print(f"  {'Parameter':<24} | {'Default':<12} | {'Type / Range':<22} | {'Description'}")
        print("  " + "-" * 76)
        for p_name, p_info in sec_data["params"].items():
            print(f"  {p_name:<24} | {str(p_info['default']):<12} | {str(p_info['type']):<22} | {p_info['desc']}")
    print("\n" + "=" * 80)
    print("CLI Shortcut Aliases (use with --set <alias>=<value>):")
    print("  e.g. --set rob=128 --set width=8 --set l1d_size=64KB --set bp=TAGE")
    print("=" * 80)

def list_elfs():
    print("=" * 80)
    print("              Available Built-in Benchmark ELFs in TinyCpuSim                ")
    print("=" * 80)
    print(f"{'ELF Target':<25} | {'Workload Characteristic & Pattern'}")
    print("-" * 80)
    for elf_name, desc in ELF_CATALOG.items():
        print(f"{elf_name:<25} | {desc}")
    print("=" * 80)

def parse_perf_output(output_text):
    stats = {}
    m = re.search(r'Simulated Total Cycles:\s+(\d+)', output_text)
    if m: stats['cycles'] = int(m.group(1))
    
    m = re.search(r'Total Committed Insts:\s+(\d+)', output_text)
    if m: stats['insts'] = int(m.group(1))

    m = re.search(r'Total Committed uOps:\s+(\d+)', output_text)
    if m: stats['uops'] = int(m.group(1))

    m = re.search(r'Aggregate Throughput \(IPC\):\s*([\d\.]+)', output_text)
    if m: stats['ipc'] = float(m.group(1))

    m = re.search(r'uOp IPC:\s*([\d\.]+)', output_text)
    if m: stats['uop_ipc'] = float(m.group(1))

    m = re.search(r'uOp Ratio:\s*([\d\.]+)x', output_text)
    if m: stats['uop_ratio'] = float(m.group(1))

    m = re.search(r'Branch Predictions:\s+(\d+)\s+\(Accuracy:\s*([\d\.]+)%\)', output_text)
    if m:
        stats['branch_preds'] = int(m.group(1))
        stats['branch_acc'] = float(m.group(2))

    m = re.search(r'Branch Mispredicts:\s+(\d+)\s+\(Penalty Flushes:\s*(\d+)\)', output_text)
    if m:
        stats['branch_mispredicts'] = int(m.group(1))
        stats['branch_flushes'] = int(m.group(2))
    else:
        m2 = re.search(r'Branch Mispredicts:\s+(\d+)', output_text)
        if m2: stats['branch_mispredicts'] = int(m2.group(1))

    m = re.search(r'BTB Hits / Misses:\s+\d+\s+/\s+\d+\s+\(Hit Rate:\s*([\d\.]+)%\)', output_text)
    if m: stats['btb_hit_rate'] = float(m.group(1))

    m = re.search(r'RAS Hits / Misses:\s+\d+\s+/\s+\d+\s+\(Hit Rate:\s*([\d\.]+)%\)', output_text)
    if m: stats['ras_hit_rate'] = float(m.group(1))

    m = re.search(r'L1I Cache Hit Rate:\s*([\d\.]+)%', output_text)
    if m: stats['l1i_hit_rate'] = float(m.group(1))

    m = re.search(r'L1D Cache Hit Rate:\s*([\d\.]+)%', output_text)
    if m: stats['l1d_hit_rate'] = float(m.group(1))

    m = re.search(r'L2 Hits / Misses:\s+\d+\s+/\s+\d+\s+\(Hit Rate:\s*([\d\.]+)%\)', output_text)
    if m: stats['l2_hit_rate'] = float(m.group(1))

    m = re.search(r'Loads / Stores:\s+(\d+)\s+/\s+(\d+)', output_text)
    if m:
        stats['loads'] = int(m.group(1))
        stats['stores'] = int(m.group(2))

    m = re.search(r'Store-to-Load Forwards:\s+(\d+)(?:\s+\(Rate:\s*([\d\.]+)%\))?', output_text)
    if m:
        stats['store_forwards'] = int(m.group(1))
        if m.group(2): stats['forwarding_rate'] = float(m.group(2))

    m = re.search(r'Mem Order Violations:\s+(\d+)', output_text)
    if m: stats['mem_order_violations'] = int(m.group(1))

    m = re.search(r'ROB Full Stalls:\s+(\d+)', output_text)
    if m: stats['rob_stalls'] = int(m.group(1))

    m = re.search(r'RS/IQ Full Stalls:\s+(\d+)', output_text)
    if m: stats['rs_stalls'] = int(m.group(1))

    m = re.search(r'PRF Exhaustion Stalls:\s+(\d+)', output_text)
    if m: stats['prf_stalls'] = int(m.group(1))

    m = re.search(r'LQ / SQ Full Stalls:\s+(\d+)\s+/\s+(\d+)', output_text)
    if m:
        stats['lq_stalls'] = int(m.group(1))
        stats['sq_stalls'] = int(m.group(2))

    m = re.search(r'Head-of-ROB Stalls:\s+(\d+)', output_text)
    if m: stats['head_rob_stalls'] = int(m.group(1))

    m = re.search(r'Frontend Bound:\s*([\d\.]+)%', output_text)
    if m: stats['frontend_bound'] = float(m.group(1))

    m = re.search(r'Bad Speculation:\s*([\d\.]+)%', output_text)
    if m: stats['bad_spec'] = float(m.group(1))

    m = re.search(r'Backend Bound:\s*([\d\.]+)%', output_text)
    if m: stats['backend_bound'] = float(m.group(1))

    m = re.search(r'Retiring:\s*([\d\.]+)%', output_text)
    if m: stats['retiring'] = float(m.group(1))

    m = re.search(r'Port ALU uOps:\s+(\d+)', output_text)
    if m: stats['port_alu'] = int(m.group(1))

    m = re.search(r'Port Branch uOps:\s+(\d+)', output_text)
    if m: stats['port_branch'] = int(m.group(1))

    m = re.search(r'Port LSU uOps:\s+(\d+)', output_text)
    if m: stats['port_lsu'] = int(m.group(1))

    m = re.search(r'L1D MSHR Allocations:\s+(\d+)\s+\(Stalls:\s*(\d+)\)', output_text)
    if m:
        stats['l1d_mshr_allocs'] = int(m.group(1))
        stats['l1d_mshr_stalls'] = int(m.group(2))

    return stats

def generate_config_with_overrides(base_path, overrides):
    content = ""
    if base_path and os.path.exists(base_path):
        with open(base_path, 'r') as f:
            content = f.read()
    else:
        content = """[simulation]
elf_path = tests/fixtures/test_fibonacci.elf
mode = uarch
all_perf = true
enable_topdown = true

[system]
num_cores = 1
dram_latency_cycles = 80
enable_mesi_coherence = true

[core]
enable_ooo = true
fetch_width = 4
decode_width = 4
rename_width = 4
issue_width = 4
commit_width = 4
rob_size = 64
rs_size = 32
num_phys_regs = 128

[branch_predictor]
enabled = true
type = TAGE
table_size = 4096
btb_size = 4096
ras_size = 32
tage_tables = 4

[lsu]
enabled = true
lq_size = 16
sq_size = 16
enable_store_forwarding = true
enable_speculative_load = true
store_forward_latency = 1

[cache_l1i]
enabled = true
size_bytes = 32768
associativity = 4
line_size = 64
hit_latency_cycles = 1
mshr_entries = 8

[cache_l1d]
enabled = true
size_bytes = 32768
associativity = 4
line_size = 64
hit_latency_cycles = 1
mshr_entries = 8

[cache_l2]
enabled = true
size_bytes = 524288
associativity = 8
line_size = 64
hit_latency_cycles = 10
mshr_entries = 16
"""
    lines = content.splitlines()
    for sec, key, val in overrides:
        found = False
        new_lines = []
        in_section = False
        for line in lines:
            stripped = line.strip()
            if stripped.startswith('[') and stripped.endswith(']'):
                in_section = (stripped[1:-1] == sec)
            if in_section and (stripped.startswith(f"{key} =") or stripped.startswith(f"{key}=")):
                new_lines.append(f"{key} = {val}")
                found = True
            else:
                new_lines.append(line)
        if not found:
            new_lines.append(f"\n[{sec}]\n{key} = {val}")
        lines = new_lines

    tmp = tempfile.NamedTemporaryFile(mode='w', suffix='.cfg', delete=False)
    tmp.write("\n".join(lines))
    tmp.close()
    return tmp.name

def parse_ini_sections(file_path):
    config = configparser.ConfigParser(strict=False, inline_comment_prefixes=('#', ';'))
    if not file_path or not os.path.exists(file_path):
        return config
    try:
        config.read(file_path)
    except Exception:
        pass
    return config

def diff_configs(base_path, exp_path):
    base_cfg = parse_ini_sections(base_path)
    exp_cfg = parse_ini_sections(exp_path)
    
    diffs = []
    all_secs = sorted(list(set(base_cfg.sections()) | set(exp_cfg.sections())))
    for sec in all_secs:
        if sec in ["workload", "simulation"]:
            continue
        base_items = dict(base_cfg.items(sec)) if base_cfg.has_section(sec) else {}
        exp_items = dict(exp_cfg.items(sec)) if exp_cfg.has_section(sec) else {}
        
        all_keys = sorted(list(set(base_items.keys()) | set(exp_items.keys())))
        for k in all_keys:
            v_base = base_items.get(k, "<unset>")
            v_exp = exp_items.get(k, "<unset>")
            if v_base != v_exp:
                diffs.append((sec, k, v_base, v_exp))
    return diffs

def get_elf_from_config(cfg_path):
    if not cfg_path or not os.path.exists(cfg_path):
        return None
    config = parse_ini_sections(cfg_path)
    for sec in ["workload", "simulation"]:
        if config.has_section(sec) and config.has_option(sec, "elf_path"):
            p = config.get(sec, "elf_path").strip()
            if p:
                return p
    return None

def format_report_header(elf_path, cfg_file, overrides_list=None):
    import datetime
    now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    header = [
        "================================================================================",
        "                  TinyCpuSim uArch Simulation Performance Report                ",
        "================================================================================",
        f"  Target Program:        {elf_path}",
        f"  Generation Timestamp:  {now_str}",
        f"  Applied Configuration: {cfg_file}",
    ]
    if overrides_list:
        header.append("  Active Hardware Overrides:")
        for sec, key, old_v, new_v in overrides_list:
            header.append(f"    • [{sec}] {key} = {new_v}")
    header.append("--------------------------------------------------------------------------------")
    return "\n".join(header) + "\n\n"

def run_sim(sim_bin, elf_file, cfg_file):
    cmd = [sim_bin, "--uarch", "--uarch-config", cfg_file, "--all-perf", elf_file]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return parse_perf_output(res.stdout), res.stdout

def format_row(label, v_base, v_exp, unit="", lower_better=False, is_pct=False):
    delta_str = "---"
    if isinstance(v_base, (int, float)) and isinstance(v_exp, (int, float)):
        if v_base > 0:
            delta = ((v_exp - v_base) / float(v_base)) * 100.0
            if abs(delta) < 0.001:
                delta_str = "0.0%"
            else:
                sign = "+" if delta > 0 else ""
                # Coloring
                if lower_better:
                    color = "\033[1;32m" if delta < 0 else ("\033[1;31m" if delta > 0 else "")
                else:
                    color = "\033[1;32m" if delta > 0 else ("\033[1;31m" if delta < 0 else "")
                delta_str = f"{color}{sign}{delta:.2f}%\033[0m"
        elif v_base == 0 and v_exp > 0:
            delta_str = f"\033[1;31m+{v_exp}\033[0m" if lower_better else f"\033[1;32m+{v_exp}\033[0m"

    fmt_base = f"{v_base:.2f}{unit}" if isinstance(v_base, float) else f"{v_base}{unit}" if v_base != 'N/A' else 'N/A'
    fmt_exp = f"{v_exp:.2f}{unit}" if isinstance(v_exp, float) else f"{v_exp}{unit}" if v_exp != 'N/A' else 'N/A'
    return f"  {label:<34} | {fmt_base:<13} | {fmt_exp:<13} | {delta_str:<12}"

def print_single_run_table(elf_path, exp_stats, changed_params, exp_cfg_path):
    print("\n" + "=" * 84)
    print(f"       TinyCpuSim Microarchitectural Simulation Report: [{os.path.basename(elf_path)}]")
    print("=" * 84)
    print(f"  Target Program:        {elf_path}")
    print(f"  Baseline Report:       [None in reports/default/{os.path.basename(elf_path).replace('.elf', '.txt')}]")
    print(f"  Applied Configuration: {exp_cfg_path}")
    print("-" * 84)
    
    if changed_params:
        print("  Active Hardware Parameter Modifications (vs Default):")
        for sec, key, old_v, new_v in changed_params:
            print(f"    • [{sec}] {key}: {old_v}  -->  \033[1;32m{new_v}\033[0m")
        print("-" * 84)

    # 1. Overall Core Performance
    print(f"  {'1. Overall Core Performance':<38} | {'Value':<18}")
    print("  " + "-" * 60)
    print(f"  {'Simulated Total Cycles':<38} | {str(exp_stats.get('cycles', 'N/A')):<18}")
    print(f"  {'Throughput (IPC)':<38} | {exp_stats.get('ipc', 0.0):.3f}")
    print(f"  {'uOp Throughput (uOp IPC)':<38} | {exp_stats.get('uop_ipc', 0.0):.3f}")
    print(f"  {'Committed Instructions':<38} | {str(exp_stats.get('insts', 'N/A')):<18}")
    print(f"  {'Committed uOps':<38} | {str(exp_stats.get('uops', 'N/A')):<18}")

    # 2. Top-Down TMAM Breakdown
    print("\n  " + "-" * 60)
    print(f"  {'2. Top-Down TMAM Breakdown':<38} | {'Pipeline Slots (%)':<18}")
    print("  " + "-" * 60)
    print(f"  {'Retiring (Useful Work)':<38} | {exp_stats.get('retiring', 0.0):.2f}%")
    print(f"  {'Bad Speculation (Squashed)':<38} | {exp_stats.get('bad_spec', 0.0):.2f}%")
    print(f"  {'Front-End Bound (Fetch/BTB)':<38} | {exp_stats.get('frontend_bound', 0.0):.2f}%")
    print(f"  {'Back-End Bound (Stalls)':<38} | {exp_stats.get('backend_bound', 0.0):.2f}%")

    # 3. Pipeline Stalls & Hazards
    print("\n  " + "-" * 60)
    print(f"  {'3. Pipeline Stalls & Hazards':<38} | {'Stall Cycles':<18}")
    print("  " + "-" * 60)
    print(f"  {'Issue Queue (RS) Full Stalls':<38} | {exp_stats.get('rs_stalls', 0)} cyc")
    print(f"  {'ROB Full Stalls':<38} | {exp_stats.get('rob_stalls', 0)} cyc")
    print(f"  {'PRF FreeList Exhaustion Stalls':<38} | {exp_stats.get('prf_stalls', 0)} cyc")
    print(f"  {'LQ / SQ Full Stalls':<38} | {exp_stats.get('lq_stalls', 0)} / {exp_stats.get('sq_stalls', 0)} cyc")
    print(f"  {'Head-of-ROB Stalls':<38} | {exp_stats.get('head_rob_stalls', 0)} cyc")

    # 4. Branch Predictor & Control
    print("\n  " + "-" * 60)
    print(f"  {'4. Branch Predictor & Control':<38} | {'Value':<18}")
    print("  " + "-" * 60)
    print(f"  {'Branch Predictor Accuracy':<38} | {exp_stats.get('branch_acc', 0.0):.2f}%")
    print(f"  {'Branch Mispredict Penalty Flushes':<38} | {exp_stats.get('branch_mispredicts', 0)}")
    print(f"  {'BTB Target Hit Rate':<38} | {exp_stats.get('btb_hit_rate', 0.0):.2f}%")
    print(f"  {'RAS Return Hit Rate':<38} | {exp_stats.get('ras_hit_rate', 0.0):.2f}%")

    # 5. Memory & Cache Subsystem
    print("\n  " + "-" * 60)
    print(f"  {'5. Memory & Cache Subsystem':<38} | {'Value':<18}")
    print("  " + "-" * 60)
    print(f"  {'L1I Cache Hit Rate':<38} | {exp_stats.get('l1i_hit_rate', 0.0):.2f}%")
    print(f"  {'L1D Cache Hit Rate':<38} | {exp_stats.get('l1d_hit_rate', 0.0):.2f}%")
    print(f"  {'Shared L2 Cache Hit Rate':<38} | {exp_stats.get('l2_hit_rate', 0.0):.2f}%")
    print(f"  {'Store-to-Load Bypass Rate':<38} | {exp_stats.get('forwarding_rate', 0.0):.2f}%")
    print(f"  {'L1D MSHR Saturation Stalls':<38} | {exp_stats.get('l1d_mshr_stalls', 0)}")
    print("=" * 84)

def print_comparison_table(elf_name, changed_params, base_stats, exp_stats, baseline_report_path, exp_cfg_path):
    print("\n" + "=" * 84)
    print(f"       TinyCpuSim Microarchitectural Experiment Report: [{os.path.basename(elf_name)}]")
    print("=" * 84)
    print(f"  Target Program:        {elf_name}")
    print(f"  Baseline Report:       {baseline_report_path}")
    print(f"  Applied Configuration: {exp_cfg_path}")
    print("-" * 84)
    
    if changed_params:
        print("  Active Hardware Parameter Modifications (vs Baseline):")
        for sec, key, old_v, new_v in changed_params:
            print(f"    • [{sec}] {key}: {old_v}  -->  \033[1;32m{new_v}\033[0m")
        print("-" * 84)
    else:
        print("  Active Hardware Parameters: (Matches Default Baseline Configuration)")
        print("-" * 84)

    # 1. Executive Summary
    c_base = base_stats.get('cycles', 0)
    c_exp = exp_stats.get('cycles', 0)
    ipc_base = base_stats.get('ipc', 0.0)
    ipc_exp = exp_stats.get('ipc', 0.0)

    if c_base > 0 and c_exp > 0:
        speedup = float(c_base) / float(c_exp)
        if speedup >= 1.0:
            verdict = f"\033[1;32mSPEEDUP: {speedup:.2f}x\033[0m (+{((speedup-1)*100):.1f}% faster)"
        else:
            slowdown = float(c_exp) / float(c_base)
            verdict = f"\033[1;31mSLOWDOWN: {slowdown:.2f}x\033[0m ({((1.0 - speedup)*100):.1f}% slower)"
        print(f"  Executive Impact: {verdict} | Baseline IPC: {ipc_base:.3f} -> Exp IPC: {ipc_exp:.3f}")
        print("-" * 84)

    print(f"  {'1. Overall Core Performance':<34} | {'Baseline':<13} | {'Experiment':<13} | {'Delta (%)':<12}")
    print("  " + "-" * 80)
    print(format_row("Simulated Total Cycles", base_stats.get('cycles', 'N/A'), exp_stats.get('cycles', 'N/A'), lower_better=True))
    print(format_row("Throughput (IPC)", base_stats.get('ipc', 'N/A'), exp_stats.get('ipc', 'N/A'), lower_better=False))
    print(format_row("uOp Throughput (uOp IPC)", base_stats.get('uop_ipc', 'N/A'), exp_stats.get('uop_ipc', 'N/A'), lower_better=False))
    print(format_row("Committed Instructions", base_stats.get('insts', 'N/A'), exp_stats.get('insts', 'N/A'), lower_better=False))
    print(format_row("Committed uOps", base_stats.get('uops', 'N/A'), exp_stats.get('uops', 'N/A'), lower_better=False))

    # 2. TMAM Breakdown
    print("\n  " + "-" * 80)
    print(f"  {'2. Top-Down TMAM Breakdown':<34} | {'Baseline':<13} | {'Experiment':<13} | {'Delta (%)':<12}")
    print("  " + "-" * 80)
    print(format_row("Retiring (Useful Work)", base_stats.get('retiring', 'N/A'), exp_stats.get('retiring', 'N/A'), "%", lower_better=False))
    print(format_row("Bad Speculation (Squashed)", base_stats.get('bad_spec', 'N/A'), exp_stats.get('bad_spec', 'N/A'), "%", lower_better=True))
    print(format_row("Front-End Bound (Fetch/BTB)", base_stats.get('frontend_bound', 'N/A'), exp_stats.get('frontend_bound', 'N/A'), "%", lower_better=True))
    print(format_row("Back-End Bound (Stalls)", base_stats.get('backend_bound', 'N/A'), exp_stats.get('backend_bound', 'N/A'), "%", lower_better=True))

    # 3. Pipeline Stalls
    print("\n  " + "-" * 80)
    print(f"  {'3. Pipeline Stalls & Hazards':<34} | {'Baseline':<13} | {'Experiment':<13} | {'Delta (%)':<12}")
    print("  " + "-" * 80)
    print(format_row("Issue Queue (RS) Full Stalls", base_stats.get('rs_stalls', 'N/A'), exp_stats.get('rs_stalls', 'N/A'), " cyc", lower_better=True))
    print(format_row("ROB Full Stalls", base_stats.get('rob_stalls', 'N/A'), exp_stats.get('rob_stalls', 'N/A'), " cyc", lower_better=True))
    print(format_row("PRF FreeList Exhaustion Stalls", base_stats.get('prf_stalls', 'N/A'), exp_stats.get('prf_stalls', 'N/A'), " cyc", lower_better=True))
    print(format_row("LQ / SQ Full Stalls", base_stats.get('lq_stalls', 'N/A'), exp_stats.get('lq_stalls', 'N/A'), " cyc", lower_better=True))
    print(format_row("Head-of-ROB Stalls", base_stats.get('head_rob_stalls', 'N/A'), exp_stats.get('head_rob_stalls', 'N/A'), " cyc", lower_better=True))

    # 4. Branch Predictor
    print("\n  " + "-" * 80)
    print(f"  {'4. Branch Predictor & Control':<34} | {'Baseline':<13} | {'Experiment':<13} | {'Delta (%)':<12}")
    print("  " + "-" * 80)
    print(format_row("Branch Predictor Accuracy", base_stats.get('branch_acc', 'N/A'), exp_stats.get('branch_acc', 'N/A'), "%", lower_better=False))
    print(format_row("Branch Mispredict Penalty Flushes", base_stats.get('branch_mispredicts', 'N/A'), exp_stats.get('branch_mispredicts', 'N/A'), "", lower_better=True))
    print(format_row("BTB Target Hit Rate", base_stats.get('btb_hit_rate', 'N/A'), exp_stats.get('btb_hit_rate', 'N/A'), "%", lower_better=False))
    print(format_row("RAS Return Hit Rate", base_stats.get('ras_hit_rate', 'N/A'), exp_stats.get('ras_hit_rate', 'N/A'), "%", lower_better=False))

    # 5. LSU & Memory Hierarchy
    print("\n  " + "-" * 80)
    print(f"  {'5. Memory & Cache Subsystem':<34} | {'Baseline':<13} | {'Experiment':<13} | {'Delta (%)':<12}")
    print("  " + "-" * 80)
    print(format_row("L1I Cache Hit Rate", base_stats.get('l1i_hit_rate', 'N/A'), exp_stats.get('l1i_hit_rate', 'N/A'), "%", lower_better=False))
    print(format_row("L1D Cache Hit Rate", base_stats.get('l1d_hit_rate', 'N/A'), exp_stats.get('l1d_hit_rate', 'N/A'), "%", lower_better=False))
    print(format_row("Shared L2 Cache Hit Rate", base_stats.get('l2_hit_rate', 'N/A'), exp_stats.get('l2_hit_rate', 'N/A'), "%", lower_better=False))
    print(format_row("Store-to-Load Bypass Rate", base_stats.get('forwarding_rate', 'N/A'), exp_stats.get('forwarding_rate', 'N/A'), "%", lower_better=False))
    print(format_row("L1D MSHR Saturation Stalls", base_stats.get('l1d_mshr_stalls', 'N/A'), exp_stats.get('l1d_mshr_stalls', 'N/A'), "", lower_better=True))

    # 6. Automated Architectural Takeaways
    print("\n" + "=" * 84)
    print("  🔍 Automated Architectural Diagnosis & Key Takeaways:")
    insights = []
    
    if c_base > 0 and c_exp > 0:
        if c_exp < c_base:
            ipc_gain = ((ipc_exp - ipc_base) / ipc_base) * 100.0
            insights.append(f"• \033[1;32mThroughput Gain\033[0m: Execution speedup is +{((c_base-c_exp)/float(c_exp))*100:.1f}%, IPC improved by +{ipc_gain:.1f}%.")
        elif c_exp > c_base:
            ipc_loss = ((ipc_base - ipc_exp) / ipc_base) * 100.0
            insights.append(f"• \033[1;31mPerformance Degradation\033[0m: Execution took {((c_exp-c_base)/float(c_base))*100:.1f}% more cycles, IPC dropped by -{ipc_loss:.1f}%.")

    rs_diff = exp_stats.get('rs_stalls', 0) - base_stats.get('rs_stalls', 0)
    if rs_diff > 50:
        insights.append(f"• \033[1;33mIssue Queue Bottleneck\033[0m: Issue Queue / RS stalls increased by +{rs_diff} cycles (in-order serialization / dependency stalls).")

    rob_diff = exp_stats.get('rob_stalls', 0) - base_stats.get('rob_stalls', 0)
    if rob_diff > 50:
        insights.append(f"• \033[1;33mROB Saturation\033[0m: ROB capacity limit caused +{rob_diff} stall cycles.")

    flush_diff = exp_stats.get('branch_mispredicts', 0) - base_stats.get('branch_mispredicts', 0)
    if flush_diff > 5:
        insights.append(f"• \033[1;33mBranch Speculation Penalty\033[0m: Branch mispredict flushes increased by +{flush_diff}, worsening pipeline recovery overhead.")
    elif flush_diff < -5:
        insights.append(f"• \033[1;32mBranch Improvement\033[0m: Eliminated {abs(flush_diff)} branch mispredict flushes, smoothing frontend supply.")

    be_diff = exp_stats.get('backend_bound', 0.0) - base_stats.get('backend_bound', 0.0)
    if be_diff > 10.0:
        insights.append(f"• \033[1;33mBackend Bound Shift\033[0m: Pipeline shifted +{be_diff:.1f}% deeper into Backend Bound slots.")

    if not insights:
        insights.append("• Performance metrics remained stable across baseline and experiment.")

    for ins in insights:
        print(f"  {ins}")
    print("=" * 84)

def main():
    parser = argparse.ArgumentParser(
        description="TinyCpuSim Microarchitectural Experiment & Parameter Tuning Tool",
        formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument("target_elf", nargs="?", help="Optional target ELF binary (defaults to active config or test_fibonacci.elf)")
    parser.add_argument("--elf", "-e", help="Target ELF binary name or comma-separated list (e.g. test_fibonacci.elf or test_fibonacci,test_sort)")
    parser.add_argument("--exp-config", "-ec", help="Experiment configuration file (defaults to active configs/current.cfg)")
    parser.add_argument("--config", "-c", help="Baseline configuration file or snapshot name in configs/save/ or configs/default/")
    parser.add_argument("--set", "-s", action="append", help="Override hardware parameter: --set <param>=<value>\n(e.g. --set rob=128 --set width=8 --set l1d_size=64KB --set bp=TAGE)")
    parser.add_argument("--base-report", "-br", help="Optional baseline report text file to compare against")
    parser.add_argument("--list-params", "-lp", action="store_true", help="List all tunable microarchitecture parameters with descriptions")
    parser.add_argument("--list-elfs", "-le", action="store_true", help="List all available built-in test benchmark ELF files")
    parser.add_argument("--jobs", "-j", type=int, default=max(1, (os.cpu_count() or 4) // 2), help=f"Parallel worker threads/processes for multi-ELF batches (default: {max(1, (os.cpu_count() or 4) // 2)}, half of CPU cores)")
    parser.add_argument("--output", "-o", help="Optional report file path to save output")
    args = parser.parse_args()

    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sim_bin = os.path.join(root_dir, "build", "tinycpusim")
    fixtures_dir = os.path.join(root_dir, "tests", "fixtures")
    reports_dir = os.path.join(root_dir, "reports")
    default_reports_dir = os.path.join(reports_dir, "default")
    configs_dir = os.path.join(root_dir, "configs")
    os.makedirs(reports_dir, exist_ok=True)
    os.makedirs(default_reports_dir, exist_ok=True)

    if args.list_params:
        list_parameters()
        sys.exit(0)

    if args.list_elfs:
        list_elfs()
        sys.exit(0)

    # 1. Resolve Target ELFs
    raw_elf_str = args.elf or args.target_elf
    current_cfg_path = os.path.join(configs_dir, "current.cfg")
    if not raw_elf_str:
        cfg_elf = get_elf_from_config(current_cfg_path)
        if cfg_elf:
            raw_elf_str = cfg_elf
        else:
            raw_elf_str = "test_fibonacci.elf"

    elf_targets = []
    for item in raw_elf_str.split(','):
        item = item.strip()
        if not item: continue
        if not item.endswith('.elf'): item += '.elf'
        full_path = item if os.path.isabs(item) else os.path.join(fixtures_dir, os.path.basename(item))
        if not os.path.exists(full_path):
            cand = os.path.join(root_dir, item)
            if os.path.exists(cand):
                full_path = cand
            else:
                print(f"Error: ELF fixture not found: {full_path}")
                print("Run 'python3 scripts/experiment.py --list-elfs' to view available ELFs.")
                sys.exit(1)
        elf_targets.append(full_path)

    # 2. Resolve Baseline Configuration
    base_cfg = None
    if args.config:
        cfg_cand = args.config
        if not cfg_cand.endswith('.cfg') and not os.path.exists(cfg_cand):
            cfg_cand += '.cfg'
        
        search_paths = [
            cfg_cand,
            os.path.join(configs_dir, cfg_cand),
            os.path.join(configs_dir, "save", cfg_cand),
            os.path.join(configs_dir, "default", cfg_cand),
        ]
        for p in search_paths:
            if os.path.exists(p):
                base_cfg = p
                break
        if not base_cfg:
            print(f"Error: Baseline config file not found for '{args.config}'. Checked: {search_paths}")
            sys.exit(1)
    else:
        default_base = os.path.join(configs_dir, "default", "default.cfg")
        if not os.path.exists(default_base):
            default_base = os.path.join(configs_dir, "current.cfg")
        base_cfg = default_base

    # 3. Resolve Experiment Configuration
    if args.exp_config:
        exp_source_cfg = args.exp_config
    else:
        exp_source_cfg = current_cfg_path if os.path.exists(current_cfg_path) else base_cfg

    if not os.path.exists(sim_bin):
        print(f"Simulator binary not found. Running build first...")
        subprocess.run([os.path.join(root_dir, "scripts", "01_build.sh")], check=True)

    # 4. Process Parameter Overrides
    overrides = []
    changed_params = []
    is_temp_cfg = False
    if args.set:
        is_temp_cfg = True
        for item in args.set:
            if '=' not in item:
                print(f"Warning: Invalid override format: '{item}'. Expected format is 'key=value'.")
                continue
            k, v = item.split('=', 1)
            k = k.strip().lower()
            v = v.strip()
            
            sec = "core"
            actual_key = k
            if k in PARAM_ALIASES:
                sec, actual_key = PARAM_ALIASES[k]
            else:
                for s_name, s_info in PARAM_CATALOG.items():
                    if k in s_info["params"]:
                        sec = s_name
                        actual_key = k
                        break

            if actual_key in ["size_bytes", "l1i_size", "l1d_size", "l2_size"] and any(c.isalpha() for c in v):
                v_num = parse_byte_size(v)
                v = str(v_num)

            if actual_key == "issue_width" and k in ["width", "issue_width"]:
                overrides.append(("core", "fetch_width", v))
                overrides.append(("core", "decode_width", v))
                overrides.append(("core", "issue_width", v))
                overrides.append(("core", "commit_width", v))
                changed_params.append(("core", "pipeline_width (fetch/decode/issue/commit)", "default", v))
            else:
                overrides.append((sec, actual_key, v))
                changed_params.append((sec, actual_key, "default", v))

        exp_cfg = generate_config_with_overrides(exp_source_cfg, overrides)
    else:
        exp_cfg = exp_source_cfg
        changed_params = diff_configs(base_cfg, exp_cfg)

    import concurrent.futures

    def _sim_worker(target_path):
        elf_base = os.path.basename(target_path).replace('.elf', '')
        default_base_report = os.path.join(default_reports_dir, f"{elf_base}.txt")

        has_baseline = False
        baseline_report_path = None
        if args.base_report and os.path.exists(args.base_report):
            has_baseline = True
            baseline_report_path = args.base_report
        elif os.path.exists(default_base_report):
            has_baseline = True
            baseline_report_path = default_base_report

        exp_stats, exp_log = run_sim(sim_bin, target_path, exp_cfg)

        out_file = args.output
        if not out_file:
            import datetime
            ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            out_file = os.path.join(reports_dir, f"exp_{elf_base}_{ts}.txt")

        with open(out_file, 'w') as f_out:
            f_out.write(format_report_header(target_path, exp_cfg, changed_params))
            f_out.write(exp_log)

        base_stats = None
        if has_baseline:
            with open(baseline_report_path, 'r') as f_br:
                base_stats = parse_perf_output(f_br.read())

        return {
            'elf_path': target_path,
            'elf_base': elf_base,
            'has_baseline': has_baseline,
            'baseline_report_path': baseline_report_path,
            'exp_stats': exp_stats,
            'base_stats': base_stats,
            'out_file': out_file
        }

    try:
        if len(elf_targets) > 1:
            print(f"Running {len(elf_targets)} simulations in parallel using {args.jobs} workers (half of CPU cores)...")
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
                results = list(executor.map(_sim_worker, elf_targets))
        else:
            results = [_sim_worker(elf_targets[0])]

        for r in results:
            elf_path = r['elf_path']
            elf_base = r['elf_base']
            exp_stats = r['exp_stats']
            out_file = r['out_file']

            if r['has_baseline']:
                print_comparison_table(elf_path, changed_params, r['base_stats'], exp_stats, r['baseline_report_path'], exp_cfg)
                print(f"  Experiment performance report saved to: \033[1;36m{out_file}\033[0m")
            else:
                print_single_run_table(elf_path, exp_stats, changed_params, exp_cfg)
                print(f"  Performance report saved to: \033[1;36m{out_file}\033[0m\n")
                print("  \033[1;33m💡 [Baseline Setup]:\033[0m")
                print(f"  No baseline report was found for this ELF in \033[1mreports/default/{elf_base}.txt\033[0m.")
                print("  To establish this simulation as the default baseline, copy this report directly:")
                print(f"    \033[1;32mcp {out_file} reports/default/{elf_base}.txt\033[0m")
                print("=" * 84)

    finally:
        if is_temp_cfg and os.path.exists(exp_cfg):
            try:
                os.remove(exp_cfg)
            except OSError:
                pass

if __name__ == '__main__':
    main()
