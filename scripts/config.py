#!/usr/bin/python3
"""
TinyCpuSim Configuration Lifecycle Manager (scripts/config.py)
Direct vi editor integration, clean preset loading from default/, save/, and sweep/,
automatic KB/MB unit conversion, and parameter validation.
"""

import os
import sys
import argparse
import configparser
import shutil
import re
import glob
import subprocess

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIGS_DIR = os.path.join(PROJECT_ROOT, "configs")
DEFAULT_DIR = os.path.join(CONFIGS_DIR, "default")
SAVE_DIR = os.path.join(CONFIGS_DIR, "save")
SWEEP_DIR = os.path.join(CONFIGS_DIR, "sweep")
CURRENT_CFG = os.path.join(CONFIGS_DIR, "current.cfg")

def ensure_dirs():
    os.makedirs(DEFAULT_DIR, exist_ok=True)
    os.makedirs(SAVE_DIR, exist_ok=True)
    os.makedirs(SWEEP_DIR, exist_ok=True)
    if not os.path.exists(CURRENT_CFG):
        default_src = os.path.join(DEFAULT_DIR, "default.cfg")
        if os.path.exists(default_src):
            shutil.copyfile(default_src, CURRENT_CFG)

def parse_byte_size(val_str):
    """Convert human-readable units like '32KB', '512KB', '1MB' to integer bytes."""
    val_str = str(val_str).strip().upper()
    if val_str.endswith("KB") or val_str.endswith("K"):
        num = int(re.sub(r'[^0-9]', '', val_str))
        return num * 1024
    elif val_str.endswith("MB") or val_str.endswith("M"):
        num = int(re.sub(r'[^0-9]', '', val_str))
        return num * 1024 * 1024
    elif val_str.endswith("B"):
        return int(re.sub(r'[^0-9]', '', val_str))
    try:
        return int(val_str)
    except:
        return val_str

def format_byte_size(bytes_val):
    try:
        b = int(bytes_val)
        if b >= 1048576 and b % 1048576 == 0:
            return f"{b // 1048576} MB ({b} B)"
        elif b >= 1024 and b % 1024 == 0:
            return f"{b // 1024} KB ({b} B)"
        return f"{b} B"
    except:
        return str(bytes_val)

def is_power_of_two(n):
    return n > 0 and (n & (n - 1)) == 0

def convert_and_format_file(file_path):
    """Reads INI file, converts any human-readable KB/MB in size fields to integer bytes, preserving comments."""
    if not os.path.exists(file_path): return
    with open(file_path, 'r') as f:
        lines = f.readlines()

    new_lines = []
    for line in lines:
        stripped = line.strip()
        if stripped and not stripped.startswith('#') and '=' in line:
            key, val_comment = line.split('=', 1)
            key_clean = key.strip()
            # Split off inline comments if any
            val_clean = val_comment.split('#')[0].strip()
            comment_part = "#" + val_comment.split('#')[1] if '#' in val_comment else ""
            
            if "size" in key_clean.lower() and any(c.isalpha() for c in val_clean):
                byte_val = parse_byte_size(val_clean)
                new_line = f"{key_clean} = {byte_val}  {comment_part}\n" if comment_part else f"{key_clean} = {byte_val}\n"
                new_lines.append(new_line)
                continue
        new_lines.append(line)

    with open(file_path, 'w') as f:
        f.writelines(new_lines)

def validate_file(file_path):
    """Validates configuration parameters in file_path and returns list of warnings."""
    if not os.path.exists(file_path): return ["Configuration file not found."]
    cfg = configparser.ConfigParser(strict=False, inline_comment_prefixes=('#', ';'))
    try:
        cfg.read(file_path)
    except Exception as e:
        return [f"INI Syntax Error: {e}"]

    errors = []
    if cfg.has_section("simulation"):
        elf = cfg.get("simulation", "elf_path", fallback="")
        if elf:
            elf_full = elf if os.path.isabs(elf) else os.path.join(PROJECT_ROOT, elf)
            if not os.path.exists(elf_full):
                errors.append(f"ELF binary not found: {elf}")
        mode = cfg.get("simulation", "mode", fallback="uarch")
        if mode not in ["uarch", "isa_only"]:
            errors.append(f"Invalid mode: '{mode}'. Must be 'uarch' or 'isa_only'.")

    if cfg.has_section("system"):
        coh = cfg.get("system", "coherence", fallback=cfg.get("system", "coherence_protocol", fallback="MESI")).upper()
        if coh not in ["MESI", "NONE", "DISABLED"]:
            errors.append(f"Invalid coherence protocol: '{coh}'. Must be MESI or NONE.")

    if cfg.has_section("core"):
        c_type = cfg.get("core", "type", fallback="OOO_DYNAMIC").upper()
        if c_type not in ["OOO_DYNAMIC", "OOO_RS_PRF", "OOO_TOMASULO", "OOO", "SIMPLE_INORDER", "INORDER", "FAST_FEEDER", "FEEDER", "BYPASS"]:
            errors.append(f"Invalid core type: '{c_type}'. Must be OOO_DYNAMIC, SIMPLE_INORDER, or FAST_FEEDER.")
        for w in ["fetch_width", "decode_width", "rename_width", "issue_width", "commit_width"]:
            val = cfg.getint("core", w, fallback=4)
            if val <= 0: errors.append(f"Core {w} must be > 0 (got {val})")
        rob = cfg.getint("core", "rob_size", fallback=64)
        if rob <= 0: errors.append(f"ROB size must be > 0 (got {rob})")
        prf = cfg.getint("core", "num_phys_regs", fallback=128)
        if prf <= 16: errors.append(f"Physical register count must be > 16 (got {prf})")

    if cfg.has_section("branch_predictor"):
        bp_type = cfg.get("branch_predictor", "type", fallback="TAGE").upper()
        if bp_type not in ["NONE", "IDEAL", "BIMODAL", "GSHARE", "TAGE", "DISABLED", "BYPASS"]:
            errors.append(f"Invalid branch predictor type: '{bp_type}'. Must be NONE, IDEAL, BIMODAL, GSHARE, or TAGE.")
        for table in ["table_size", "btb_size"]:
            t_val = cfg.getint("branch_predictor", table, fallback=4096)
            if not is_power_of_two(t_val):
                errors.append(f"Branch predictor {table} must be a power of 2 (got {t_val})")

    if cfg.has_section("lsu"):
        lsu_type = cfg.get("lsu", "type", fallback="SPECULATIVE_OOO").upper()
        if lsu_type not in ["SPECULATIVE_OOO", "OOO", "STRICT_INORDER", "INORDER", "PASSTHROUGH", "BYPASS", "NONE"]:
            errors.append(f"Invalid LSU type: '{lsu_type}'. Must be SPECULATIVE_OOO, STRICT_INORDER, or PASSTHROUGH.")

    for c_sec in ["cache_l1i", "cache_l1d", "cache_l2", "l1i", "l1d", "l2"]:
        if cfg.has_section(c_sec):
            c_type = cfg.get(c_sec, "type", fallback="SET_ASSOCIATIVE").upper()
            if c_type not in ["SET_ASSOCIATIVE", "DEFAULT", "DIRECT_MAPPED", "ZERO_LATENCY", "IDEAL", "PASSTHROUGH", "NONE", "BYPASS"]:
                errors.append(f"Invalid cache type for [{c_sec}]: '{c_type}'. Must be SET_ASSOCIATIVE, DIRECT_MAPPED, ZERO_LATENCY, or PASSTHROUGH.")
            if c_type != "PASSTHROUGH" and c_type != "NONE" and c_type != "BYPASS":
                c_size = cfg.getint(c_sec, "size_bytes", fallback=32768)
                c_assoc = cfg.getint(c_sec, "associativity", fallback=4)
                if not is_power_of_two(c_size):
                    errors.append(f"Cache [{c_sec}] size_bytes must be a power of 2 (got {c_size})")
                if not is_power_of_two(c_assoc):
                    errors.append(f"Cache [{c_sec}] associativity must be a power of 2 (got {c_assoc})")
            pf_type = cfg.get(c_sec, "prefetcher", fallback=cfg.get(c_sec, "prefetcher_type", fallback="NONE")).upper()
            if pf_type not in ["NONE", "DISABLED", "NEXT_LINE", "NEXTLINE", "STRIDE", "STREAM"]:
                errors.append(f"Invalid prefetcher type for [{c_sec}]: '{pf_type}'. Must be NONE, NEXT_LINE, STRIDE, or STREAM.")
            if pf_type not in ["NONE", "DISABLED"]:
                dist = cfg.getint(c_sec, "prefetch_distance", fallback=1)
                q_size = cfg.getint(c_sec, "prefetch_queue_size", fallback=8)
                if dist <= 0:
                    errors.append(f"Cache [{c_sec}] prefetch_distance must be > 0 (got {dist})")
                if q_size <= 0:
                    errors.append(f"Cache [{c_sec}] prefetch_queue_size must be > 0 (got {q_size})")

    if cfg.has_section("prefetcher"):
        pf_type = cfg.get("prefetcher", "type", fallback=cfg.get("prefetcher", "prefetcher", fallback="NONE")).upper()
        if pf_type not in ["NONE", "DISABLED", "NEXT_LINE", "NEXTLINE", "STRIDE", "STREAM"]:
            errors.append(f"Invalid prefetcher type: '{pf_type}'. Must be NONE, NEXT_LINE, STRIDE, or STREAM.")
        if pf_type not in ["NONE", "DISABLED"]:
            dist = cfg.getint("prefetcher", "prefetch_distance", fallback=cfg.getint("prefetcher", "distance", fallback=1))
            q_size = cfg.getint("prefetcher", "prefetch_queue_size", fallback=cfg.getint("prefetcher", "queue_size", fallback=8))
            if dist <= 0:
                errors.append(f"Prefetcher prefetch_distance must be > 0 (got {dist})")
            if q_size <= 0:
                errors.append(f"Prefetcher prefetch_queue_size must be > 0 (got {q_size})")

    return errors

def print_validation_status(file_path):
    convert_and_format_file(file_path)
    errors = validate_file(file_path)
    if errors:
        print("\n\033[1;31m[VALIDATION WARNING / ERROR]\033[0m")
        for err in errors:
            print(f"  ❌ {err}")
        print("\033[1;33mPlease fix invalid parameters.\033[0m\n")
    else:
        print(f"\033[1;32m[OK]\033[0m All parameters verified and valid.")
    print(f"\033[1;32m[OK]\033[0m Configuration updated at: \033[1m{file_path}\033[0m")

def edit_with_vi():
    ensure_dirs()
    editor = os.environ.get("EDITOR", "vi")
    print(f"\nOpening {CURRENT_CFG} with {editor}...")
    os.system(f"{editor} {CURRENT_CFG}")
    print_validation_status(CURRENT_CFG)

def show_config(file_path=CURRENT_CFG, title="Active Configuration"):
    if not os.path.exists(file_path):
        print(f"File not found: {file_path}")
        return
    cfg = configparser.ConfigParser(strict=False, inline_comment_prefixes=('#', ';'))
    cfg.read(file_path)

    print("\n" + "=" * 80)
    print(f"       TinyCpuSim Microarchitectural Configuration: {title}")
    print("=" * 80)
    
    if cfg.has_section("simulation"):
        print("[simulation] - Target Workload & Execution Mode")
        print(f"  Target ELF Binary:       \033[1;36m{cfg.get('simulation', 'elf_path', fallback='N/A')}\033[0m")
        print(f"  Simulation Mode:         {cfg.get('simulation', 'mode', fallback='uarch')} (Cycle-Accurate uArch)")
        print(f"  Max Instruction Steps:   {cfg.get('simulation', 'max_steps', fallback='1000000000')}")
        print(f"  Detailed Hardware Log:   {cfg.get('simulation', 'all_perf', fallback='true')}")
        print(f"  Top-Down TMAM Profiler:  {cfg.get('simulation', 'enable_topdown', fallback='true')}")
        print("-" * 80)

    if cfg.has_section("system") or cfg.has_section("global"):
        sec = "system" if cfg.has_section("system") else "global"
        print("[system] - Multicore & Interconnect Subsystem")
        print(f"  Core Count:              {cfg.get(sec, 'num_cores', fallback='1')} core(s)")
        print(f"  Off-chip DRAM Latency:   {cfg.get(sec, 'dram_latency_cycles', fallback=cfg.get(sec, 'dram_latency', fallback='80'))} clock cycles")
        coh = cfg.get(sec, 'coherence', fallback=cfg.get(sec, 'enable_mesi_coherence', fallback='MESI'))
        print(f"  Coherence Protocol:      \033[1;32m{coh}\033[0m")
        print("-" * 80)

    if cfg.has_section("core"):
        print("[core] - Superscalar Pipeline & Execution Engine")
        c_type = cfg.get('core', 'type', fallback='OOO_DYNAMIC').upper()
        print(f"  Module Type:             \033[1;32m{c_type}\033[0m")
        print(f"  Stage Widths (F/D/R/I/C):{cfg.get('core', 'fetch_width', fallback='4')} Fetch / {cfg.get('core', 'decode_width', fallback='4')} Decode / {cfg.get('core', 'rename_width', fallback='4')} Rename / {cfg.get('core', 'issue_width', fallback='4')} Issue / {cfg.get('core', 'commit_width', fallback='4')} Commit")
        print(f"  Reorder Buffer (ROB):    {cfg.get('core', 'rob_size', fallback='64')} entries")
        print(f"  Issue Queue / RS:        {cfg.get('core', 'rs_size', fallback='32')} entries")
        print(f"  Physical Registers (PRF):{cfg.get('core', 'num_phys_regs', fallback='128')} physical registers (16 arch regs)")
        print("-" * 80)

    if cfg.has_section("branch_predictor"):
        print("[branch_predictor] - Branch Prediction Unit (BPU)")
        bp_type = cfg.get('branch_predictor', 'type', fallback='TAGE').upper()
        print(f"  Module Type:             \033[1;33m{bp_type}\033[0m (PHT Capacity: {cfg.get('branch_predictor', 'table_size', fallback='4096')} entries)")
        print(f"  Branch Target Buffer:    {cfg.get('branch_predictor', 'btb_size', fallback='4096')} entries")
        print(f"  Return Address Stack:    {cfg.get('branch_predictor', 'ras_size', fallback='32')} entries")
        print(f"  TAGE Geometric Tables:   {cfg.get('branch_predictor', 'tage_tables', fallback='4')} tables")
        print("-" * 80)

    if cfg.has_section("lsu"):
        print("[lsu] - Load/Store Unit & Memory Disambiguation")
        lsu_type = cfg.get('lsu', 'type', fallback='SPECULATIVE_OOO').upper()
        print(f"  Module Type:             \033[1;32m{lsu_type}\033[0m")
        print(f"  Load / Store Queues:     LQ: {cfg.get('lsu', 'lq_size', fallback='16')} entries | SQ: {cfg.get('lsu', 'sq_size', fallback='16')} entries")
        print(f"  Store-to-Load Bypass:    Latency: {cfg.get('lsu', 'store_forward_latency', fallback='1')} cycle")
        print("-" * 80)

    print("[caches] - Multi-Level Memory Hierarchy")
    for sec, name in [("cache_l1i", "L1 Instruction Cache"), ("l1i", "L1 Instruction Cache"),
                      ("cache_l1d", "L1 Data Cache"), ("l1d", "L1 Data Cache"),
                      ("cache_l2", "Shared L2 Cache"), ("l2", "Shared L2 Cache")]:
        if cfg.has_section(sec):
            c_type = cfg.get(sec, 'type', fallback='SET_ASSOCIATIVE').upper()
            c_sz = format_byte_size(cfg.get(sec, 'size_bytes', fallback='32768'))
            c_as = cfg.get(sec, 'associativity', fallback='4')
            c_ln = cfg.get(sec, 'line_size', fallback='64')
            c_lat = cfg.get(sec, 'hit_latency_cycles', fallback=cfg.get(sec, 'hit_latency', fallback='1'))
            c_mshr = cfg.get(sec, 'mshr_entries', fallback='8')
            c_pf = cfg.get(sec, 'prefetcher', fallback=cfg.get(sec, 'prefetcher_type', fallback='NONE')).upper()
            pf_str = f" | Prefetch: {c_pf}" if c_pf not in ["NONE", "DISABLED"] else ""
            print(f"  {name:<24} Type: {c_type:<16} | {c_sz} | {c_as}-way | Block: {c_ln}B | Latency: {c_lat} cyc | MSHRs: {c_mshr}{pf_str}")
    print("=" * 80)

def run_sweep(sweep_cfg_files=None):
    if not sweep_cfg_files:
        sweep_cfg_files = sorted(glob.glob(os.path.join(SWEEP_DIR, "*.cfg")))
    
    if not sweep_cfg_files:
        print("No sweep configurations found in configs/sweep/.")
        print("Use option [5] 'Save to sweep/' to create sweep configurations.")
        return

    print("=" * 76)
    print(f" Running Batch Sweep across {len(sweep_cfg_files)} configuration files")
    print("=" * 76)

    # Check for ELF mismatches
    elf_map = {}
    for f in sweep_cfg_files:
        c = configparser.ConfigParser()
        c.read(f)
        elf = c.get("simulation", "elf_path", fallback="tests/fixtures/test_fibonacci.elf")
        elf_map[f] = elf

    unique_elfs = set(elf_map.values())
    if len(unique_elfs) > 1:
        print("\n\033[1;33m⚠️  [WARNING: MISMATCHED ELF PATHS DETECTED ACROSS SWEEP CONFIGS]\033[0m")
        print("  Sweep Mode is intended to compare different hardware configurations on the SAME program.")
        print("  Detected conflicting ELF paths:")
        for cfg_file, e_path in elf_map.items():
            print(f"    - {os.path.basename(cfg_file)}: {e_path}")
        
        curr_cfg = configparser.ConfigParser()
        curr_cfg.read(CURRENT_CFG)
        target_elf = curr_cfg.get("simulation", "elf_path", fallback="tests/fixtures/test_fibonacci.elf")
        print(f"\nUnifying all sweep runs to the current active ELF binary: \033[1;32m{target_elf}\033[0m\n")
    else:
        target_elf = list(unique_elfs)[0]
        print(f"  Target ELF for all sweep runs: \033[1;32m{target_elf}\033[0m\n")

    sim_bin = os.path.join(PROJECT_ROOT, "build", "tinycpusim")
    if not os.path.exists(sim_bin):
        subprocess.run([os.path.join(PROJECT_ROOT, "scripts", "01_build.sh")], check=True)

    elf_full = target_elf if os.path.isabs(target_elf) else os.path.join(PROJECT_ROOT, target_elf)

    print(f"{'Config Name':<20} | {'Cycles':<10} | {'IPC':<8} | {'Branch Acc':<12} | {'L1D Hit Rate':<12}")
    print("-" * 76)

    for cfg_path in sweep_cfg_files:
        cmd = [sim_bin, "--uarch", "--uarch-config", cfg_path, elf_full]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        out = res.stdout
        
        m_c = re.search(r'Simulated Total Cycles:\s+(\d+)', out)
        cycles = m_c.group(1) if m_c else 'N/A'
        
        m_ipc = re.search(r'Aggregate Throughput \(IPC\):\s*([\d\.]+)', out)
        ipc = m_ipc.group(1) if m_ipc else 'N/A'
        
        m_ba = re.search(r'Branch Predictions:\s+\d+\s+\(Accuracy:\s*([\d\.]+)%\)', out)
        b_acc = f"{m_ba.group(1)}%" if m_ba else 'N/A'
        
        m_l1d = re.search(r'L1D Cache Hit Rate:\s*([\d\.]+)%', out)
        l1d = f"{m_l1d.group(1)}%" if m_l1d else 'N/A'

        print(f"{os.path.basename(cfg_path):<20} | {cycles:<10} | {ipc:<8} | {b_acc:<12} | {l1d:<12}")

    print("=" * 76)

def interactive_menu():
    ensure_dirs()
    while True:
        cfg = configparser.ConfigParser()
        cfg.read(CURRENT_CFG)
        target_elf = cfg.get('simulation', 'elf_path', fallback='tests/fixtures/test_fibonacci.elf')
        width = cfg.get('core', 'issue_width', fallback='4')
        rob = cfg.get('core', 'rob_size', fallback='64')
        bp = cfg.get('branch_predictor', 'type', fallback='TAGE')
        cores = cfg.get('system', 'num_cores', fallback='1')

        print("\n============================================================")
        print("          TinyCpuSim Configuration Manager (TUI)            ")
        print("============================================================")
        print(f" Active: \033[1m{CURRENT_CFG}\033[0m")
        print(f" Target: \033[1;36m{target_elf}\033[0m ({width}-wide | ROB: {rob} | BPU: {bp} | Cores: {cores})")
        print("------------------------------------------------------------")
        print("  [1] Edit Active Config with vi (Direct vi / $EDITOR)")
        print("  [2] Load from default/ (Load single-core / multi-core presets)")
        print("  [3] Load from save/ (Load custom saved snapshot)")
        print("  [4] Save to save/ (Save active config snapshot to configs/save/)")
        print("  [5] Save to sweep/ (Save active config to configs/sweep/ for batch sweep)")
        print("  [6] View Active Config (Display active hardware parameters)")
        print("  [0] Exit")
        print("============================================================")
        choice = input("Enter choice [0-6]: ").strip()

        if choice == "1":
            edit_with_vi()
        elif choice == "2":
            print("\nDefault Presets in configs/default/:")
            defaults = sorted(glob.glob(os.path.join(DEFAULT_DIR, "*.cfg")))
            for i, d in enumerate(defaults, 1):
                print(f"  [{i}] {os.path.basename(d)}")
            sel = input("Select preset number: ").strip()
            if sel.isdigit() and 1 <= int(sel) <= len(defaults):
                chosen = defaults[int(sel)-1]
                shutil.copyfile(chosen, CURRENT_CFG)
                print_validation_status(CURRENT_CFG)
        elif choice == "3":
            saved = sorted(glob.glob(os.path.join(SAVE_DIR, "*.cfg")))
            if not saved:
                print("No saved configurations found in configs/save/.")
            else:
                print("\nSaved Configurations in configs/save/:")
                for i, s_file in enumerate(saved, 1):
                    print(f"  [{i}] {os.path.basename(s_file)}")
                sel = input("Select saved config number: ").strip()
                if sel.isdigit() and 1 <= int(sel) <= len(saved):
                    chosen = saved[int(sel)-1]
                    shutil.copyfile(chosen, CURRENT_CFG)
                    print_validation_status(CURRENT_CFG)
        elif choice == "4":
            name = input("Enter snapshot name to save in configs/save/ (e.g. my_opt_v1): ").strip()
            if name:
                if not name.endswith('.cfg'): name += '.cfg'
                dest = os.path.join(SAVE_DIR, name)
                shutil.copyfile(CURRENT_CFG, dest)
                print_validation_status(dest)
        elif choice == "5":
            name = input("Enter sweep config name to save in configs/sweep/ (e.g. sweep_rob128): ").strip()
            if name:
                if not name.endswith('.cfg'): name += '.cfg'
                dest = os.path.join(SWEEP_DIR, name)
                shutil.copyfile(CURRENT_CFG, dest)
                print_validation_status(dest)
        elif choice == "6":
            show_config(CURRENT_CFG, f"Active ({CURRENT_CFG})")
        elif choice in ["0", "q", "Q"]:
            print(f"\nExiting. Active configuration ready at: \033[1m{CURRENT_CFG}\033[0m")
            break

def main():
    ensure_dirs()
    parser = argparse.ArgumentParser(description="TinyCpuSim Configuration Manager")
    subparsers = parser.add_subparsers(dest="command")

    # edit
    subparsers.add_parser("edit", help="Edit active configuration directly with vi")

    # show
    subparsers.add_parser("show", help="Display the active configuration table")

    # set
    set_parser = subparsers.add_parser("set", help="Set a configuration key (e.g. set core issue_width 8)")
    set_parser.add_argument("section", help="INI Section (e.g. core, branch_predictor, cache_l1d)")
    set_parser.add_argument("key", help="Configuration key name")
    set_parser.add_argument("value", help="Value to assign")

    # save
    save_parser = subparsers.add_parser("save", help="Save active configuration to configs/save/<name>.cfg")
    save_parser.add_argument("name", help="Snapshot filename")

    # save-sweep
    sweep_save_parser = subparsers.add_parser("save-sweep", help="Save active configuration to configs/sweep/<name>.cfg")
    sweep_save_parser.add_argument("name", help="Sweep filename")

    # load
    load_parser = subparsers.add_parser("load", help="Load configuration from configs/save/ or configs/default/")
    load_parser.add_argument("name", help="Filename to load")

    # reset
    subparsers.add_parser("reset", help="Reset active configuration to default template")

    # list
    subparsers.add_parser("list", help="List all configs in default/, save/, and sweep/")

    # sweep
    sweep_parser = subparsers.add_parser("sweep", help="Run batch sweep evaluation across configs/sweep/")
    sweep_parser.add_argument("configs", nargs="*", help="Optional list of config files")

    # menu
    subparsers.add_parser("menu", help="Launch interactive TUI menu")

    args = parser.parse_args()

    if not args.command or args.command == "menu":
        interactive_menu()
        return

    if args.command == "edit":
        edit_with_vi()

    elif args.command == "show":
        show_config(CURRENT_CFG, f"Active ({CURRENT_CFG})")

    elif args.command == "set":
        cfg = configparser.ConfigParser(strict=False, inline_comment_prefixes=('#', ';'))
        cfg.read(CURRENT_CFG)
        if not cfg.has_section(args.section):
            cfg.add_section(args.section)
        cfg.set(args.section, args.key, args.value)
        with open(CURRENT_CFG, 'w') as fp:
            cfg.write(fp)
        print(f"Updated [{args.section}] {args.key} = {args.value} in {CURRENT_CFG}")
        print_validation_status(CURRENT_CFG)

    elif args.command == "save":
        name = args.name
        if not name.endswith('.cfg'): name += '.cfg'
        dest = os.path.join(SAVE_DIR, name)
        shutil.copyfile(CURRENT_CFG, dest)
        print_validation_status(dest)

    elif args.command == "save-sweep":
        name = args.name
        if not name.endswith('.cfg'): name += '.cfg'
        dest = os.path.join(SWEEP_DIR, name)
        shutil.copyfile(CURRENT_CFG, dest)
        print_validation_status(dest)

    elif args.command == "load":
        name = args.name
        if not name.endswith('.cfg'): name += '.cfg'
        src = os.path.join(SAVE_DIR, name)
        if not os.path.exists(src):
            src = os.path.join(DEFAULT_DIR, name)
        if os.path.exists(src):
            shutil.copyfile(src, CURRENT_CFG)
            print_validation_status(CURRENT_CFG)
        else:
            print(f"Error: {name} not found in configs/save/ or configs/default/.")

    elif args.command == "reset":
        shutil.copyfile(os.path.join(DEFAULT_DIR, "default.cfg"), CURRENT_CFG)
        print_validation_status(CURRENT_CFG)

    elif args.command == "list":
        print("=" * 60)
        print("          TinyCpuSim Configuration Repository               ")
        print("=" * 60)
        print("  [default/]")
        for f in sorted(glob.glob(os.path.join(DEFAULT_DIR, "*.cfg"))):
            print(f"    - {os.path.basename(f)}")
        print("  [save/]")
        saved = sorted(glob.glob(os.path.join(SAVE_DIR, "*.cfg")))
        if not saved: print("    (No saved configurations)")
        for f in saved:
            print(f"    - {os.path.basename(f)}")
        print("  [sweep/]")
        sweeps = sorted(glob.glob(os.path.join(SWEEP_DIR, "*.cfg")))
        if not sweeps: print("    (No sweep configurations)")
        for f in sweeps:
            print(f"    - {os.path.basename(f)}")
        print("=" * 60)

    elif args.command == "sweep":
        run_sweep(args.configs)

if __name__ == '__main__':
    main()
