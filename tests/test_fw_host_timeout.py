"""Tests for host timeout and process cleanup harness."""

import os
import signal
import sys
import unittest
from pathlib import Path

from scripts.firmware_test_harness import FirmwareTimeoutError, run_bounded


class FirmwareHostTimeoutTest(unittest.TestCase):
    def test_run_bounded_success(self) -> None:
        cmd = [sys.executable, "-c", 'print("ok")']
        res = run_bounded(cmd, timeout=2.0)
        self.assertEqual(res.returncode, 0)
        self.assertEqual(res.stdout, "ok\n")
        self.assertEqual(res.stderr, "")

    def test_run_bounded_preserves_exit_code(self) -> None:
        cmd = [sys.executable, "-c", "import sys; sys.exit(7)"]
        res = run_bounded(cmd, timeout=2.0)
        self.assertEqual(res.returncode, 7)

    def test_run_bounded_timeout_kills_process_group_and_reaps_pid(self) -> None:
        cmd = [
            sys.executable,
            "-c",
            'import time, sys; sys.stdout.write("started\\n"); sys.stdout.flush(); time.sleep(5)',
        ]
        with self.assertRaises(FirmwareTimeoutError) as ctx:
            run_bounded(cmd, timeout=0.1)

        err = ctx.exception
        self.assertIsInstance(err, TimeoutError)
        self.assertIn("started", err.stdout)
        self.assertIsNotNone(err.pid)

        # Direct PID must be gone
        pid = err.pid
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_run_bounded_real_simulator_max_steps_timeout(self) -> None:
        repo_root = Path(__file__).resolve().parent.parent
        sim_path = repo_root / "build" / "tinycpusim"
        elf_path = repo_root / "build" / "isa_boot_acceptance.elf"
        self.assertTrue(sim_path.exists(), f"Simulator binary not found at {sim_path}")
        self.assertTrue(elf_path.exists(), f"ELF binary not found at {elf_path}")

        cmd = [str(sim_path), "--firmware", "--max-steps", "1", str(elf_path)]
        res = run_bounded(cmd, timeout=2.0)
        self.assertEqual(res.returncode, 1)
        combined_output = res.stdout + res.stderr
        self.assertIn("Simulation execution exceeded max step limit (1)", combined_output)
        self.assertNotIn("No ELF input", combined_output)


if __name__ == "__main__":
    unittest.main()
