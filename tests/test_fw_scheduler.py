"""Tests for firmware static scheduler, timer callbacks and one-second heartbeat (Slices 13, 14, 15, 16)."""

import pathlib
import re
import subprocess
import unittest

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


class TestFirmwareScheduler(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sim = _REPO_ROOT / "build" / "tinyarmsim"
        cls.elf_release = _REPO_ROOT / "build" / "scheduler_acceptance.elf"
        cls.elf_debug = _REPO_ROOT / "build" / "scheduler_acceptance_debug.elf"
        if not cls.sim.exists():
            raise unittest.SkipTest("tinyarmsim executable missing")
        if not cls.elf_release.exists() or not cls.elf_debug.exists():
            raise unittest.SkipTest("scheduler acceptance elfs missing")

    def test_scheduler_acceptance_release(self):
        cmd = [
            str(self.sim),
            "--firmware",
            "--max-steps",
            "500000",
            str(self.elf_release),
        ]
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(result.returncode, 0, f"Sim failed with stderr:\n{result.stderr}")
        self.assertIn("SCHED_START", result.stdout)
        self.assertEqual(result.stdout.count("[HB]"), 3, "Expected exactly 3 heartbeat messages")
        self.assertIn("SCHEDULER_ACCEPTANCE_PASS", result.stdout)

    def test_scheduler_acceptance_debug(self):
        cmd = [
            str(self.sim),
            "--firmware",
            "--max-steps",
            "500000",
            str(self.elf_debug),
        ]
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(result.returncode, 0, f"Sim failed with stderr:\n{result.stderr}")
        self.assertIn("SCHED_START", result.stdout)
        self.assertEqual(result.stdout.count("[HB]"), 3, "Expected exactly 3 heartbeat messages")
        self.assertIn("SCHEDULER_ACCEPTANCE_PASS", result.stdout)

    def test_scheduler_diagnostic_storage_stripped_in_release(self):
        cmd = ["arm-none-eabi-size", str(self.elf_release), str(self.elf_debug)]
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(result.returncode, 0)
        lines = result.stdout.strip().splitlines()
        self.assertGreaterEqual(len(lines), 3)

        # Parse BSS column (index 2)
        match_rel = re.search(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+.*scheduler_acceptance\.elf", lines[1])
        match_dbg = re.search(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+.*scheduler_acceptance_debug\.elf", lines[2])
        self.assertIsNotNone(match_rel)
        self.assertIsNotNone(match_dbg)

        bss_release = int(match_rel.group(3))
        bss_debug = int(match_dbg.group(3))

        # Diagnostic structure (fw_task_diag_t diags[4]) occupies 48 bytes in debug BSS
        self.assertEqual(bss_debug - bss_release, 48, "Debug build must allocate exactly 48 bytes for task diags")
        self.assertLess(bss_release, bss_debug, "Release build must strip diagnostic storage")


if __name__ == "__main__":
    unittest.main()
