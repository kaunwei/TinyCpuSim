#!/usr/bin/python3
"""
TinyCpuSim vs gem5 Multi-Slice Performance & Drift Point Comparator (scripts/compare_slices.py)

Parses multi-slice / multi-interval gem5 stats and TinyCpuSim slice reports
(supporting JSON, Gem5, and Text slice formats), computes interval-by-interval
IPC, Branch, and Cache error deltas, and automatically identifies microarchitectural
drift points with root-cause attribution.
"""

import os
import sys
import re
import json
import argparse
import subprocess
import tempfile
from typing import List, Dict, Any, Optional, Tuple

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(PROJECT_ROOT, "build")
SIM_BIN = os.path.join(BUILD_DIR, "tinycpusim")
GOLDEN_DIR = os.path.join(PROJECT_ROOT, "tests", "golden", "gem5")
FIXTURES_DIR = os.path.join(PROJECT_ROOT, "tests", "fixtures")


class SliceRecord:
    def __init__(self, slice_id: int):
        self.slice_id = slice_id
        self.start_cycle = 0
        self.end_cycle = 0
        self.cycles = 0
        self.start_inst = 0
        self.end_inst = 0
        self.insts = 0
        self.uops = 0
        self.ipc = 0.0
        
        # Branch predictor
        self.bp_lookups = 0
        self.bp_mispredicts = 0
        self.bp_accuracy = 100.0
        self.btb_hits = 0
        self.btb_lookups = 0
        self.ras_used = 0
        self.ras_correct = 0

        # Caches
        self.icache_accesses = 0
        self.icache_hits = 0
        self.icache_misses = 0
        self.icache_hit_rate = 100.0

        self.dcache_accesses = 0
        self.dcache_hits = 0
        self.dcache_misses = 0
        self.dcache_hit_rate = 100.0

        self.l2_accesses = 0
        self.l2_hits = 0
        self.l2_misses = 0
        self.l2_hit_rate = 100.0

        # Execution / LSU
        self.loads = 0
        self.stores = 0
        self.mem_violations = 0
        self.int_alu_ops = 0

    def compute_rates(self):
        if self.cycles > 0 and self.ipc == 0.0 and self.insts > 0:
            self.ipc = float(self.insts) / float(self.cycles)
        if self.bp_lookups > 0:
            self.bp_accuracy = 100.0 * (1.0 - (float(self.bp_mispredicts) / float(self.bp_lookups)))
        if self.icache_accesses > 0:
            self.icache_hit_rate = 100.0 * (float(self.icache_hits) / float(self.icache_accesses))
        if self.dcache_accesses > 0:
            self.dcache_hit_rate = 100.0 * (float(self.dcache_hits) / float(self.dcache_accesses))
        if self.l2_accesses > 0:
            self.l2_hit_rate = 100.0 * (float(self.l2_hits) / float(self.l2_accesses))

    def to_dict(self) -> Dict[str, Any]:
        return {
            "slice_id": self.slice_id,
            "start_cycle": self.start_cycle,
            "end_cycle": self.end_cycle,
            "cycles": self.cycles,
            "start_inst": self.start_inst,
            "end_inst": self.end_inst,
            "insts": self.insts,
            "uops": self.uops,
            "ipc": round(self.ipc, 6),
            "branch": {
                "lookups": self.bp_lookups,
                "mispredicts": self.bp_mispredicts,
                "accuracy": round(self.bp_accuracy, 2),
                "btb_hits": self.btb_hits,
                "btb_lookups": self.btb_lookups,
                "ras_used": self.ras_used,
                "ras_correct": self.ras_correct,
            },
            "cache": {
                "icache_accesses": self.icache_accesses,
                "icache_hits": self.icache_hits,
                "icache_hit_rate": round(self.icache_hit_rate, 2),
                "dcache_accesses": self.dcache_accesses,
                "dcache_hits": self.dcache_hits,
                "dcache_hit_rate": round(self.dcache_hit_rate, 2),
                "l2_accesses": self.l2_accesses,
                "l2_hits": self.l2_hits,
                "l2_hit_rate": round(self.l2_hit_rate, 2),
            },
            "lsu": {
                "loads": self.loads,
                "stores": self.stores,
                "mem_violations": self.mem_violations,
            }
        }


def parse_gem5_dump_block(block_text: str, slice_id: int) -> SliceRecord:
    rec = SliceRecord(slice_id)
    raw = {}
    for line in block_text.splitlines():
        line = line.strip()
        if not line or line.startswith('#') or line.startswith('-'):
            continue
        parts = line.split('#')[0].split()
        if len(parts) >= 2:
            key, val_str = parts[0], parts[1]
            try:
                if '.' in val_str:
                    raw[key] = float(val_str)
                else:
                    raw[key] = int(val_str)
            except ValueError:
                raw[key] = val_str

    rec.insts = raw.get('simInsts', raw.get('sim_insts', raw.get('system.cpu_cluster.cpus.commitStats0.numInsts', 0)))
    rec.uops = raw.get('simOps', raw.get('sim_ops', raw.get('system.cpu_cluster.cpus.commitStats0.numOps', 0)))
    rec.cycles = raw.get('system.cpu_cluster.cpus.numCycles', raw.get('system.cpu.numCycles', 0))
    if 'system.cpu_cluster.cpus.ipc' in raw:
        rec.ipc = float(raw['system.cpu_cluster.cpus.ipc'])
    elif 'system.cpu.ipc' in raw:
        rec.ipc = float(raw['system.cpu.ipc'])
    elif rec.cycles > 0:
        rec.ipc = float(rec.insts) / float(rec.cycles)

    # Branch Predictor
    rec.bp_lookups = raw.get('system.cpu_cluster.cpus.branchPred.lookups_0::total',
                             raw.get('system.cpu_cluster.cpus.branchPred.condPredicted',
                             raw.get('system.cpu_cluster.cpus.branchPred.lookups', 0)))
    rec.bp_mispredicts = raw.get('system.cpu_cluster.cpus.branchPred.mispredicted_0::total',
                                 raw.get('system.cpu_cluster.cpus.branchPred.condIncorrect', 0))
    rec.btb_hits = raw.get('system.cpu_cluster.cpus.branchPred.BTBHits', 0)
    rec.btb_lookups = raw.get('system.cpu_cluster.cpus.branchPred.BTBLookups', 0)
    rec.ras_used = raw.get('system.cpu_cluster.cpus.branchPred.ras.used',
                           raw.get('system.cpu_cluster.cpus.branchPred.RASUsed', 0))
    rec.ras_correct = raw.get('system.cpu_cluster.cpus.branchPred.ras.correct', 0)

    # Caches
    rec.icache_accesses = raw.get('system.cpu_cluster.cpus.icache.overallAccesses::total',
                                  raw.get('system.cpu_cluster.cpus.icache.demandAccesses::total', 0))
    rec.icache_hits = raw.get('system.cpu_cluster.cpus.icache.overallHits::total',
                              raw.get('system.cpu_cluster.cpus.icache.demandHits::total', 0))
    rec.icache_misses = raw.get('system.cpu_cluster.cpus.icache.overallMisses::total',
                                raw.get('system.cpu_cluster.cpus.icache.demandMisses::total', 0))

    rec.dcache_accesses = raw.get('system.cpu_cluster.cpus.dcache.overallAccesses::total',
                                  raw.get('system.cpu_cluster.cpus.dcache.demandAccesses::total', 0))
    rec.dcache_hits = raw.get('system.cpu_cluster.cpus.dcache.overallHits::total',
                              raw.get('system.cpu_cluster.cpus.dcache.demandHits::total', 0))
    rec.dcache_misses = raw.get('system.cpu_cluster.cpus.dcache.overallMisses::total',
                                raw.get('system.cpu_cluster.cpus.dcache.demandMisses::total', 0))

    rec.l2_accesses = raw.get('system.cpu_cluster.l2.overallAccesses::total', 0)
    rec.l2_hits = raw.get('system.cpu_cluster.l2.overallHits::total', 0)
    rec.l2_misses = raw.get('system.cpu_cluster.l2.overallMisses::total', 0)

    # LSU
    rec.loads = raw.get('system.cpu_cluster.cpus.MemDepUnit__0.insertedLoads', 0)
    rec.stores = raw.get('system.cpu_cluster.cpus.MemDepUnit__0.insertedStores', 0)
    rec.mem_violations = raw.get('system.cpu_cluster.cpus.iew.memOrderViolationEvents', 0)
    rec.int_alu_ops = raw.get('system.cpu_cluster.cpus.intAluAccesses', 0)

    rec.compute_rates()
    return rec


def parse_gem5_stats(filepath_or_content: str, cumulative_deltas: bool = True) -> List[SliceRecord]:
    """Parse gem5 stats file containing one or more simulation statistics dump blocks."""
    content = filepath_or_content
    if os.path.exists(filepath_or_content):
        with open(filepath_or_content, 'r') as fp:
            content = fp.read()

    blocks = []
    # Split by standard gem5 delimiter
    pattern = r'---------- Begin Simulation Statistics ----------(.*?)(?:---------- End Simulation Statistics   ----------|$)'
    matches = re.findall(pattern, content, re.DOTALL)
    if matches:
        for m in matches:
            if m.strip():
                blocks.append(m.strip())
    else:
        # Single block without explicit begin/end headers
        if content.strip():
            blocks.append(content.strip())

    if not blocks:
        return []

    raw_slices = [parse_gem5_dump_block(b, i) for i, b in enumerate(blocks)]

    # If multiple dumps and cumulative_deltas is True, check if counters are cumulative and convert to deltas
    if len(raw_slices) > 1 and cumulative_deltas:
        # Check if insts are strictly increasing (cumulative dump)
        is_cumulative = all(raw_slices[i].insts <= raw_slices[i+1].insts for i in range(len(raw_slices)-1))
        if is_cumulative and raw_slices[-1].insts > raw_slices[0].insts:
            delta_slices = []
            prev = None
            cum_inst = 0
            cum_cyc = 0
            for i, cur in enumerate(raw_slices):
                d = SliceRecord(i)
                d.start_inst = prev.insts if prev else 0
                d.end_inst = cur.insts
                d.insts = cur.insts - (prev.insts if prev else 0)

                d.start_cycle = prev.cycles if prev else 0
                d.end_cycle = cur.cycles
                d.cycles = cur.cycles - (prev.cycles if prev else 0)

                d.uops = cur.uops - (prev.uops if prev else 0)
                d.ipc = (float(d.insts) / float(d.cycles)) if d.cycles > 0 else 0.0

                d.bp_lookups = max(0, cur.bp_lookups - (prev.bp_lookups if prev else 0))
                d.bp_mispredicts = max(0, cur.bp_mispredicts - (prev.bp_mispredicts if prev else 0))
                d.btb_hits = max(0, cur.btb_hits - (prev.btb_hits if prev else 0))
                d.btb_lookups = max(0, cur.btb_lookups - (prev.btb_lookups if prev else 0))
                d.ras_used = max(0, cur.ras_used - (prev.ras_used if prev else 0))
                d.ras_correct = max(0, cur.ras_correct - (prev.ras_correct if prev else 0))

                d.icache_accesses = max(0, cur.icache_accesses - (prev.icache_accesses if prev else 0))
                d.icache_hits = max(0, cur.icache_hits - (prev.icache_hits if prev else 0))
                d.icache_misses = max(0, cur.icache_misses - (prev.icache_misses if prev else 0))

                d.dcache_accesses = max(0, cur.dcache_accesses - (prev.dcache_accesses if prev else 0))
                d.dcache_hits = max(0, cur.dcache_hits - (prev.dcache_hits if prev else 0))
                d.dcache_misses = max(0, cur.dcache_misses - (prev.dcache_misses if prev else 0))

                d.l2_accesses = max(0, cur.l2_accesses - (prev.l2_accesses if prev else 0))
                d.l2_hits = max(0, cur.l2_hits - (prev.l2_hits if prev else 0))
                d.l2_misses = max(0, cur.l2_misses - (prev.l2_misses if prev else 0))

                d.loads = max(0, cur.loads - (prev.loads if prev else 0))
                d.stores = max(0, cur.stores - (prev.stores if prev else 0))
                d.mem_violations = max(0, cur.mem_violations - (prev.mem_violations if prev else 0))
                d.int_alu_ops = max(0, cur.int_alu_ops - (prev.int_alu_ops if prev else 0))

                d.compute_rates()
                delta_slices.append(d)
                prev = cur
            return delta_slices

    return raw_slices


def parse_tinysim_json_slices(content: str) -> List[SliceRecord]:
    records = []
    # Could be a single JSON list or concatenated JSON objects
    content = content.strip()
    json_objs = []
    if content.startswith('[') and content.endswith(']'):
        try:
            json_objs = json.loads(content)
        except Exception:
            pass

    if not json_objs:
        # Try matching separate { ... } blocks
        decoder = json.JSONDecoder()
        idx = 0
        length = len(content)
        while idx < length:
            while idx < length and content[idx].isspace():
                idx += 1
            if idx >= length:
                break
            try:
                obj, end_idx = decoder.raw_decode(content, idx)
                json_objs.append(obj)
                idx = end_idx
            except Exception:
                idx += 1

    for i, obj in enumerate(json_objs):
        rec = SliceRecord(obj.get('slice_id', i))
        rec.start_cycle = obj.get('start_cycle', 0)
        rec.end_cycle = obj.get('end_cycle', 0)
        rec.cycles = obj.get('slice_cycles', rec.end_cycle - rec.start_cycle)
        rec.start_inst = obj.get('start_instruction', 0)
        rec.end_inst = obj.get('end_instruction', 0)
        rec.insts = obj.get('slice_instructions', rec.end_inst - rec.start_inst)
        rec.ipc = obj.get('slice_ipc', (float(rec.insts) / float(rec.cycles) if rec.cycles > 0 else 0.0))

        delta = obj.get('delta', {})
        if delta:
            cores = delta.get('cores', [])
            if cores:
                c0 = cores[0]
                rec.uops = c0.get('committed_uops', rec.insts)
                rec.bp_lookups = c0.get('branch_predictions', 0)
                rec.bp_mispredicts = c0.get('branch_mispredicts', 0)
                rec.bp_accuracy = c0.get('branch_accuracy', 1.0) * 100.0 if c0.get('branch_accuracy', 1.0) <= 1.0 else c0.get('branch_accuracy', 100.0)
                rec.icache_hit_rate = c0.get('l1i_hit_rate', 1.0) * 100.0 if c0.get('l1i_hit_rate', 1.0) <= 1.0 else c0.get('l1i_hit_rate', 100.0)
                rec.dcache_hit_rate = c0.get('l1d_hit_rate', 1.0) * 100.0 if c0.get('l1d_hit_rate', 1.0) <= 1.0 else c0.get('l1d_hit_rate', 100.0)
                rec.loads = c0.get('loads', 0)
                rec.stores = c0.get('stores', 0)
                rec.mem_violations = c0.get('memory_order_violations', 0)
        rec.compute_rates()
        records.append(rec)
    return records


def parse_tinysim_text_slices(content: str) -> List[SliceRecord]:
    records = []
    slice_blocks = re.split(r'={50,}\n\s*TinyCpuSim Performance Slice #(\d+)', content)
    
    if len(slice_blocks) > 1:
        # Format has matches: block 0 (prefix), id 1, block 1, id 2, block 2...
        for i in range(1, len(slice_blocks), 2):
            s_id = int(slice_blocks[i])
            b_text = slice_blocks[i+1]
            rec = SliceRecord(s_id)
            
            m = re.search(r'Interval Cycles:\s+\[(\d+)\s*->\s*(\d+)\]\s*\((\d+)\s*cycles\)', b_text)
            if m:
                rec.start_cycle = int(m.group(1))
                rec.end_cycle = int(m.group(2))
                rec.cycles = int(m.group(3))

            m = re.search(r'Interval Instructions:\s+\[(\d+)\s*->\s*(\d+)\]\s*\((\d+)\s*insts\)', b_text)
            if m:
                rec.start_inst = int(m.group(1))
                rec.end_inst = int(m.group(2))
                rec.insts = int(m.group(3))

            m = re.search(r'Interval IPC:\s*([\d\.]+)', b_text)
            if m:
                rec.ipc = float(m.group(1))

            m = re.search(r'Branch Predictions:\s+(\d+)\s+\(Accuracy:\s*([\d\.]+)%\)', b_text)
            if m:
                rec.bp_lookups = int(m.group(1))
                rec.bp_accuracy = float(m.group(2))

            m = re.search(r'Branch Mispredicts:\s+(\d+)', b_text)
            if m:
                rec.bp_mispredicts = int(m.group(1))

            m = re.search(r'L1I Cache Accesses:\s+(\d+)\s+\(Hit Rate:\s*([\d\.]+)%\)', b_text)
            if m:
                rec.icache_accesses = int(m.group(1))
                rec.icache_hit_rate = float(m.group(2))

            m = re.search(r'L1D Cache Accesses:\s+(\d+)\s+\(Hit Rate:\s*([\d\.]+)%\)', b_text)
            if m:
                rec.dcache_accesses = int(m.group(1))
                rec.dcache_hit_rate = float(m.group(2))

            m = re.search(r'L2 Cache Accesses:\s+(\d+)\s+\(Hit Rate:\s*([\d\.]+)%\)', b_text)
            if m:
                rec.l2_accesses = int(m.group(1))
                rec.l2_hit_rate = float(m.group(2))

            rec.compute_rates()
            records.append(rec)
    return records


def parse_tinysim_slices(filepath_or_content: str) -> List[SliceRecord]:
    content = filepath_or_content
    if os.path.exists(filepath_or_content):
        with open(filepath_or_content, 'r') as fp:
            content = fp.read()

    # Try JSON parser first
    if '"slice_id"' in content or '"slice_ipc"' in content:
        slices = parse_tinysim_json_slices(content)
        if slices:
            return slices

    # Try gem5 format
    if '---------- Begin Simulation Statistics ----------' in content:
        slices = parse_gem5_stats(content, cumulative_deltas=False)
        if slices:
            return slices

    # Try Text format
    if 'TinyCpuSim Performance Slice #' in content:
        slices = parse_tinysim_text_slices(content)
        if slices:
            return slices

    # Fallback to single simulation summary
    rec = SliceRecord(0)
    m = re.search(r'Simulated Total Cycles:\s+(\d+)', content)
    if m: rec.cycles = int(m.group(1))
    m = re.search(r'Total Committed Insts:\s+(\d+)', content)
    if m: rec.insts = int(m.group(1))
    m = re.search(r'Aggregate Throughput \(IPC\):\s*([\d\.]+)', content)
    if m: rec.ipc = float(m.group(1))
    rec.compute_rates()
    return [rec]


def run_tinysim_with_slices(elf_path: str, interval_insts: int = 1000,
                            config_path: Optional[str] = None) -> List[SliceRecord]:
    """Runs TinyCpuSim on an ELF fixture with slice output enabled, returning parsed slices."""
    if not os.path.exists(SIM_BIN):
        raise FileNotFoundError(f"TinyCpuSim binary not found at {SIM_BIN}. Run ./scripts/01_build.sh first.")

    with tempfile.NamedTemporaryFile(mode='w+', suffix='.json', delete=False) as tmp_file:
        slice_out_path = tmp_file.name

    cmd = [
        SIM_BIN,
        "--uarch",
        "--slice-insts", str(interval_insts),
        "--slice-file", slice_out_path,
        "--slice-format", "json"
    ]
    if config_path and os.path.exists(config_path):
        cmd.extend(["--config", config_path])
    cmd.append(elf_path)

    try:
        res = subprocess.run(cmd, capture_output=True, text=True, check=True)
        slices = parse_tinysim_slices(slice_out_path)
        return slices
    finally:
        if os.path.exists(slice_out_path):
            os.remove(slice_out_path)


class IntervalComparison:
    def __init__(self, slice_id: int, tiny_slice: SliceRecord, gem5_slice: SliceRecord,
                 drift_threshold: float = 5.0):
        self.slice_id = slice_id
        self.tiny = tiny_slice
        self.gem5 = gem5_slice
        self.drift_threshold = drift_threshold

        # Calculate Error Deltas
        self.ipc_tiny = tiny_slice.ipc
        self.ipc_gem5 = gem5_slice.ipc
        if self.ipc_gem5 > 0:
            self.ipc_delta_pct = abs(self.ipc_tiny - self.ipc_gem5) / self.ipc_gem5 * 100.0
        else:
            self.ipc_delta_pct = 0.0 if self.ipc_tiny == 0 else 100.0

        if gem5_slice.cycles > 0:
            self.cycles_delta_pct = abs(tiny_slice.cycles - gem5_slice.cycles) / float(gem5_slice.cycles) * 100.0
        else:
            self.cycles_delta_pct = 0.0

        self.branch_acc_delta = abs(tiny_slice.bp_accuracy - gem5_slice.bp_accuracy)
        self.dcache_hit_delta = abs(tiny_slice.dcache_hit_rate - gem5_slice.dcache_hit_rate)
        self.icache_hit_delta = abs(tiny_slice.icache_hit_rate - gem5_slice.icache_hit_rate)

        # Drift Assessment
        self.is_drift = self.ipc_delta_pct > self.drift_threshold
        self.root_causes = self._diagnose_drift()

    def _diagnose_drift(self) -> List[str]:
        causes = []
        if not self.is_drift:
            return ["Matched"]

        if self.branch_acc_delta > 3.0:
            causes.append(f"Branch Acc Mismatch (Δ{self.branch_acc_delta:.1f}%)")
        elif self.tiny.bp_mispredicts != self.gem5.bp_mispredicts and max(self.tiny.bp_mispredicts, self.gem5.bp_mispredicts) > 5:
            causes.append(f"Branch Mispredict Count ({self.tiny.bp_mispredicts} vs {self.gem5.bp_mispredicts})")

        if self.dcache_hit_delta > 3.0:
            causes.append(f"L1D Cache Hit Mismatch (Δ{self.dcache_hit_delta:.1f}%)")

        if self.icache_hit_delta > 3.0:
            causes.append(f"L1I Cache Hit Mismatch (Δ{self.icache_hit_delta:.1f}%)")

        if not causes:
            if self.ipc_tiny > self.ipc_gem5:
                causes.append("Pipeline Throughput / Latency Optimism")
            else:
                causes.append("Pipeline Stall / Structural Bottleneck")

        return causes


def compare_slices(tiny_slices: List[SliceRecord], gem5_slices: List[SliceRecord],
                   drift_threshold: float = 5.0) -> List[IntervalComparison]:
    comparisons = []
    count = min(len(tiny_slices), len(gem5_slices))
    for i in range(count):
        cmp = IntervalComparison(i, tiny_slices[i], gem5_slices[i], drift_threshold)
        comparisons.append(cmp)
    return comparisons


def format_report_table(comparisons: List[IntervalComparison], title: str = "Slice Performance Comparison",
                        threshold: float = 5.0) -> str:
    lines = []
    lines.append("=" * 105)
    lines.append(f"     TinyCpuSim vs gem5 Multi-Slice Interval Report: [{title}]")
    lines.append(f"     Drift Gate Threshold: {threshold:.1f}% Delta")
    lines.append("=" * 105)
    header = (f"{'Slice':<6} | {'Inst Range':<17} | {'Tiny IPC':<9} | {'gem5 IPC':<9} | "
              f"{'IPC Δ(%)':<9} | {'BrAcc Δ':<8} | {'L1D Δ':<7} | {'Status':<10} | {'Diagnosis'}")
    lines.append(header)
    lines.append("-" * 105)

    drift_count = 0
    total_ipc_delta = 0.0
    max_delta = 0.0
    max_delta_slice = 0

    for c in comparisons:
        inst_range = f"[{c.tiny.start_inst}->{c.tiny.end_inst}]"
        status_str = "⚠️ DRIFT" if c.is_drift else "✅ MATCH"
        diag_str = ", ".join(c.root_causes)

        lines.append(f"#{c.slice_id:<5} | {inst_range:<17} | {c.ipc_tiny:<9.3f} | {c.ipc_gem5:<9.3f} | "
                     f"{c.ipc_delta_pct:<8.2f}% | {c.branch_acc_delta:<7.1f}% | {c.dcache_hit_delta:<6.1f}% | "
                     f"{status_str:<10} | {diag_str}")

        if c.is_drift:
            drift_count += 1
        total_ipc_delta += c.ipc_delta_pct
        if c.ipc_delta_pct > max_delta:
            max_delta = c.ipc_delta_pct
            max_delta_slice = c.slice_id

    lines.append("=" * 105)
    total_slices = len(comparisons)
    mape = (total_ipc_delta / total_slices) if total_slices > 0 else 0.0
    pass_pct = ((total_slices - drift_count) / total_slices * 100.0) if total_slices > 0 else 100.0

    lines.append(f" Summary: Total Intervals: {total_slices} | Matched: {total_slices - drift_count} ({pass_pct:.1f}%) | "
                 f"Drift Points: {drift_count} | Mean IPC Error (MAPE): {mape:.2f}%")
    if drift_count > 0:
        lines.append(f" ⚠️ Peak Drift detected at Slice #{max_delta_slice} with IPC Delta = {max_delta:.2f}%")
    else:
        lines.append(f" ✅ Microarchitectural Invariant Preserved: Zero Drift Points across all intervals (<{threshold:.1f}% delta)")
    lines.append("=" * 105)
    return "\n".join(lines)


def format_report_json(comparisons: List[IntervalComparison], title: str, threshold: float) -> str:
    total_slices = len(comparisons)
    drift_count = sum(1 for c in comparisons if c.is_drift)
    mape = sum(c.ipc_delta_pct for c in comparisons) / total_slices if total_slices > 0 else 0.0

    out = {
        "title": title,
        "drift_threshold_pct": threshold,
        "total_slices": total_slices,
        "matched_slices": total_slices - drift_count,
        "drift_slices": drift_count,
        "mean_absolute_percentage_error": round(mape, 3),
        "intervals": [
            {
                "slice_id": c.slice_id,
                "start_inst": c.tiny.start_inst,
                "end_inst": c.tiny.end_inst,
                "tiny_ipc": round(c.ipc_tiny, 4),
                "gem5_ipc": round(c.ipc_gem5, 4),
                "ipc_delta_pct": round(c.ipc_delta_pct, 2),
                "branch_accuracy_delta_pct": round(c.branch_acc_delta, 2),
                "dcache_hit_rate_delta_pct": round(c.dcache_hit_delta, 2),
                "icache_hit_rate_delta_pct": round(c.icache_hit_delta, 2),
                "is_drift": c.is_drift,
                "diagnosis": c.root_causes,
                "tiny_stats": c.tiny.to_dict(),
                "gem5_stats": c.gem5.to_dict()
            }
            for c in comparisons
        ]
    }
    return json.dumps(out, indent=2)


def main():
    parser = argparse.ArgumentParser(
        description="TinyCpuSim vs gem5 Multi-Slice Performance & Drift Point Comparator"
    )
    parser.add_argument("-g", "--gem5-stats", help="Path to gem5 stats file (single or multi-dump)")
    parser.add_argument("-t", "--tinysim-slices", help="Path to TinyCpuSim slice report (JSON, Gem5, or Text)")
    parser.add_argument("-w", "--workload", help="Workload ELF file to simulate with slices on-the-fly")
    parser.add_argument("--slice-insts", type=int, default=1000, help="Interval instructions for simulation slices (default: 1000)")
    parser.add_argument("--drift-threshold", type=float, default=5.0, help="IPC Error drift threshold in percent (default: 5.0%%)")
    parser.add_argument("--format", choices=["table", "json", "csv"], default="table", help="Report output format (default: table)")
    parser.add_argument("-o", "--output", help="Write report to file instead of stdout")
    parser.add_argument("--all-golden", action="store_true", help="Batch compare all golden test cases in tests/golden/gem5")

    args = parser.parse_args()

    # Batch mode for all golden tests
    if args.all_golden:
        golden_files = sorted(os.listdir(GOLDEN_DIR))
        overall_comparisons = {}
        all_passed = True

        for g_file in golden_files:
            if not g_file.endswith(".stats.txt"):
                continue
            case_name = g_file.replace(".stats.txt", "")
            elf_path = os.path.join(FIXTURES_DIR, f"{case_name}.elf")
            g_path = os.path.join(GOLDEN_DIR, g_file)

            if not os.path.exists(elf_path):
                continue

            gem5_slices = parse_gem5_stats(g_path)
            tiny_slices = run_tinysim_with_slices(elf_path, interval_insts=args.slice_insts)
            cmps = compare_slices(tiny_slices, gem5_slices, drift_threshold=args.drift_threshold)

            report_str = format_report_table(cmps, title=case_name, threshold=args.drift_threshold)
            print(report_str)
            print()

            if any(c.is_drift for c in cmps):
                all_passed = False

        sys.exit(0 if all_passed else 1)

    # Single comparison mode
    if args.workload:
        elf_path = args.workload
        if not os.path.exists(elf_path):
            # Try fixtures dir
            cand = os.path.join(FIXTURES_DIR, elf_path)
            if os.path.exists(cand):
                elf_path = cand
            else:
                print(f"Error: Workload ELF '{args.workload}' not found.", file=sys.stderr)
                sys.exit(1)

        tiny_slices = run_tinysim_with_slices(elf_path, interval_insts=args.slice_insts)
        
        # Auto-detect golden gem5 file if not provided
        if not args.gem5_stats:
            case_name = os.path.basename(elf_path).replace(".elf", "")
            cand_g = os.path.join(GOLDEN_DIR, f"{case_name}.stats.txt")
            if os.path.exists(cand_g):
                args.gem5_stats = cand_g
            else:
                print(f"Error: Golden gem5 file not found for {case_name}. Specify with --gem5-stats.", file=sys.stderr)
                sys.exit(1)

        gem5_slices = parse_gem5_stats(args.gem5_stats)
    elif args.tinysim_slices and args.gem5_stats:
        tiny_slices = parse_tinysim_slices(args.tinysim_slices)
        gem5_slices = parse_gem5_stats(args.gem5_stats)
    else:
        parser.print_help()
        sys.exit(0)

    cmps = compare_slices(tiny_slices, gem5_slices, drift_threshold=args.drift_threshold)
    case_name = os.path.basename(args.workload or args.tinysim_slices)

    if args.format == "json":
        report_output = format_report_json(cmps, title=case_name, threshold=args.drift_threshold)
    elif args.format == "csv":
        csv_lines = ["slice_id,start_inst,end_inst,tiny_ipc,gem5_ipc,ipc_delta_pct,branch_acc_delta,dcache_hit_delta,is_drift,diagnosis"]
        for c in cmps:
            diag_clean = ";".join(c.root_causes)
            csv_lines.append(f"{c.slice_id},{c.tiny.start_inst},{c.tiny.end_inst},{c.ipc_tiny:.4f},{c.ipc_gem5:.4f},{c.ipc_delta_pct:.2f},{c.branch_acc_delta:.2f},{c.dcache_hit_delta:.2f},{c.is_drift},{diag_clean}")
        report_output = "\n".join(csv_lines)
    else:
        report_output = format_report_table(cmps, title=case_name, threshold=args.drift_threshold)

    if args.output:
        with open(args.output, 'w') as fp:
            fp.write(report_output)
        print(f"Report written to {args.output}")
    else:
        print(report_output)

    has_drift = any(c.is_drift for c in cmps)
    sys.exit(1 if has_drift else 0)


if __name__ == "__main__":
    main()
