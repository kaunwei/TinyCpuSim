"""Tests for sustained firmware workload, timer callbacks and UART shell (Slice 21)."""

import os
import pathlib
import re
import socket
import subprocess
import sys
import time
import unittest

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from scripts.firmware_test_harness import run_service_watchdog


class TestFirmwareSustainedWorkload(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sim = _REPO_ROOT / "build" / "tinyarmsim"
        cls.elf_release = _REPO_ROOT / "build" / "reference_app.elf"
        cls.elf_debug = _REPO_ROOT / "build" / "reference_app_debug.elf"
        if not cls.sim.exists():
            raise unittest.SkipTest("tinyarmsim binary missing")
        if not cls.elf_release.exists() or not cls.elf_debug.exists():
            raise unittest.SkipTest("reference_app ELFs missing")

    def test_sustained_workload_watchdog_release(self):
        completed = run_service_watchdog(
            sim_binary=self.sim,
            elf_path=self.elf_release,
            target_service_predicate=lambda out: out.count("[HB]") >= 3,
            step_budget=500000,
            timeout_sec=10.0,
            heartbeat_token="[HB]",
        )
        self.assertEqual(completed.returncode, 0)
        self.assertIn("REFERENCE_FIRMWARE_START", completed.stdout)
        self.assertGreaterEqual(completed.stdout.count("[HB]"), 3)

    def test_sustained_workload_watchdog_debug(self):
        completed = run_service_watchdog(
            sim_binary=self.sim,
            elf_path=self.elf_debug,
            target_service_predicate=lambda out: out.count("[HB]") >= 3,
            step_budget=500000,
            timeout_sec=10.0,
            heartbeat_token="[HB]",
        )
        self.assertEqual(completed.returncode, 0)
        self.assertIn("REFERENCE_FIRMWARE_START", completed.stdout)
        self.assertGreaterEqual(completed.stdout.count("[HB]"), 3)

    def test_diagnostic_storage_stripped_in_release(self):
        cmd = ["arm-none-eabi-size", str(self.elf_release), str(self.elf_debug)]
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(result.returncode, 0)
        lines = result.stdout.strip().splitlines()
        self.assertGreaterEqual(len(lines), 3)

        match_rel = re.search(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+.*reference_app\.elf", lines[1])
        match_dbg = re.search(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+.*reference_app_debug\.elf", lines[2])
        self.assertIsNotNone(match_rel)
        self.assertIsNotNone(match_dbg)

        bss_rel = int(match_rel.group(3))
        bss_dbg = int(match_dbg.group(3))
        self.assertGreater(bss_dbg, bss_rel)
        self.assertEqual(bss_dbg - bss_rel, 48)


if __name__ == "__main__":
    unittest.main()
