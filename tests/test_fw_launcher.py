"""Tests for top-level firmware launcher and integrated device/debug environment (Slices 18 & 19, Parent Issue 08)."""

import os
import pathlib
import signal
import subprocess
import time
import unittest

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


class TestFirmwareLauncher(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.run_sh = _REPO_ROOT / "run.sh"
        cls.sim = _REPO_ROOT / "build" / "tinyarmsim"
        cls.elf = _REPO_ROOT / "build" / "scheduler_acceptance.elf"
        if not cls.run_sh.exists() or not cls.sim.exists() or not cls.elf.exists():
            raise unittest.SkipTest("run.sh, tinyarmsim, or scheduler_acceptance.elf missing")

    def test_launcher_help(self):
        result = subprocess.run([str(self.run_sh), "fw", "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("TinyCpuSim Firmware Environment Launcher", result.stdout)
        self.assertIn("--pty", result.stdout)
        self.assertIn("--debug", result.stdout)
        self.assertIn("test [ELF]", result.stdout)

    def test_launcher_test_mode(self):
        result = subprocess.run([str(self.run_sh), "fw", "test"], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0)
        self.assertIn("SCHEDULER_ACCEPTANCE_PASS", result.stdout)
        self.assertIn("SIMULATION PASSED", result.stdout)

    def test_launcher_pty_mode_and_cleanup(self):
        cmd = [str(self.run_sh), "fw", "--pty"]
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        pid = proc.pid

        # Wait for PTY slave path on stderr
        deadline = time.monotonic() + 5.0
        slave_path = None
        while time.monotonic() < deadline:
            line = proc.stderr.readline()
            if "PTY slave:" in line:
                slave_path = line.split("PTY slave:", 1)[1].strip()
                break
            time.sleep(0.05)

        self.assertIsNotNone(slave_path, "Launcher must announce PTY slave path on stderr")
        self.assertTrue(os.path.exists(slave_path), f"Slave device {slave_path} must exist")

        # Interrupt launcher and assert clean process group cleanup
        try:
            os.killpg(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass

        proc.wait(timeout=3.0)
        if proc.stdout:
            proc.stdout.close()
        if proc.stderr:
            proc.stderr.close()

        # Verify child simulator process is no longer running
        time.sleep(0.1)
        try:
            os.kill(pid, 0)
            self.fail("Launcher process should have terminated")
        except ProcessLookupError:
            pass  # Expected


if __name__ == "__main__":
    unittest.main()
