#!/usr/bin/env python3
"""
gem5 ARM SE Simulation Slice Runner with Periodic Stat Dumps.
Used by generate_gem5_slices.py and compare_slices.py to drive gem5
with periodic instruction / tick intervals matching TinyCpuSim.
"""

import argparse
import os
import sys
import shlex

import m5
from m5.objects import *
from m5.util import addToPath

# Add gem5 arm example configs path to sys.path
gem5_config_dir = os.environ.get("GEM5_CONFIG_DIR", "/home/kw/workspace/gem5/configs/example/arm")
if os.path.exists(gem5_config_dir):
    if gem5_config_dir not in sys.path:
        sys.path.insert(0, gem5_config_dir)
    m5.util.addToPath(gem5_config_dir)
    m5.util.addToPath(os.path.join(gem5_config_dir, "../.."))

import starter_se
from starter_se import cpu_types
import devices
from common import MemConfig, ObjectList


def main():
    parser = argparse.ArgumentParser(description="gem5 ARM SE Slice Simulation Runner")
    parser.add_argument("commands_to_run", metavar="command(s)", nargs="*", help="Command(s) to run")
    parser.add_argument("--cpu", type=str, choices=list(cpu_types.keys()), default="o3", help="CPU model (default: o3)")
    parser.add_argument("--cpu-freq", type=str, default="4GHz")
    parser.add_argument("--num-cores", type=int, default=1, help="Number of CPU cores")
    parser.add_argument("--mem-type", default="DDR3_1600_8x8", choices=ObjectList.mem_list.get_names(), help="Memory type")
    parser.add_argument("--mem-channels", type=int, default=2, help="Memory channels")
    parser.add_argument("--mem-ranks", type=int, default=None, help="Memory ranks per channel")
    parser.add_argument("--mem-size", type=str, default="2GiB", help="Physical memory size")
    parser.add_argument("--tarmac-gen", action="store_true", help="Write a Tarmac trace.")
    parser.add_argument("--tarmac-dest", choices=TarmacDump.vals, default="stdoutput", help="Destination for Tarmac trace")
    parser.add_argument("-P", "--param", action="append", default=[], help="SimObject parameter")
    parser.add_argument("--slice-insts", type=int, default=1000, help="Periodic instruction interval for stat dumps (default: 1000, 0 to disable)")
    parser.add_argument("--slice-ticks", type=int, default=0, help="Periodic tick interval for stat dumps (default: 0 to disable)")

    args = parser.parse_args()

    root = Root(full_system=False)
    root.system = starter_se.create(args)
    root.apply_config(args.param)

    m5.instantiate()

    if args.slice_insts > 0:
        curr_inst = 0
        interval = args.slice_insts
        while True:
            curr_inst += interval
            for cpu in root.system.cpu_cluster.cpus:
                cpu.scheduleInstStop(0, curr_inst, "slice instruction limit")
            event = m5.simulate()
            cause = event.getCause()
            m5.stats.dump()
            if cause != "slice instruction limit":
                break
    elif args.slice_ticks > 0:
        interval = args.slice_ticks
        while True:
            event = m5.simulate(interval)
            cause = event.getCause()
            m5.stats.dump()
            if cause != "simulate() limit reached":
                break
    else:
        event = m5.simulate()
        m5.stats.dump()

    print(f"Exiting @ tick {m5.curTick()} because {event.getCause()} ({event.getCode()})")


if __name__ == "__m5_main__" or __name__ == "__main__":
    main()
