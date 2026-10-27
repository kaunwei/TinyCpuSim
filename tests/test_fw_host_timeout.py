"""Tests for host timeout and process cleanup harness."""

import os
import signal
import subprocess
import sys
import tempfile
import time
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

    def test_run_bounded_caller_interruption_cleans_child_and_group(self) -> None:
        # Wrapper script calling run_bounded on a child that creates background descendants and writes PID readiness
        with tempfile.TemporaryDirectory() as tmpdir:
            ready_file = Path(tmpdir) / "ready.txt"
            pids_file = Path(tmpdir) / "pids.txt"

            wrapper_code = f"""
import os
import sys
from scripts.firmware_test_harness import run_bounded

child_code = '''
import os
import subprocess
import sys
import time
from pathlib import Path

# Spawn a grandchild in the same process group (default for Popen without new session)
gc = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
Path({repr(str(pids_file))}).write_text(f"{{os.getpid()}} {{gc.pid}}")
Path({repr(str(ready_file))}).write_text("ready")
time.sleep(30)
'''

try:
    run_bounded([sys.executable, "-c", child_code], timeout=30.0)
except KeyboardInterrupt:
    sys.exit(42)
"""
            # Launch wrapper process
            wrapper = subprocess.Popen(
                [sys.executable, "-c", wrapper_code],
                cwd=str(Path(__file__).resolve().parent.parent),
                env=os.environ.copy(),
            )

            # Bounded readiness check (poll ready_file with deadline)
            deadline = time.monotonic() + 5.0
            ready = False
            while time.monotonic() < deadline:
                if ready_file.exists() and pids_file.exists():
                    ready = True
                    break
                time.sleep(0.02)

            self.assertTrue(ready, "Timed out waiting for child readiness")

            raw_pids = pids_file.read_text().strip().split()
            child_pid = int(raw_pids[0])
            grandchild_pid = int(raw_pids[1])

            # Ensure processes are currently alive
            os.kill(child_pid, 0)
            os.kill(grandchild_pid, 0)

            # Send SIGINT ONLY to wrapper
            wrapper.send_signal(signal.SIGINT)
            exit_code = wrapper.wait(timeout=5.0)
            self.assertEqual(exit_code, 42)

            # Bounded check that child and grandchild / process group no longer live
            deadline = time.monotonic() + 3.0
            child_gone = False
            grandchild_gone = False
            while time.monotonic() < deadline:
                if not child_gone:
                    try:
                        os.kill(child_pid, 0)
                    except ProcessLookupError:
                        child_gone = True
                if not grandchild_gone:
                    try:
                        os.kill(grandchild_pid, 0)
                    except ProcessLookupError:
                        grandchild_gone = True
                if child_gone and grandchild_gone:
                    break
                time.sleep(0.02)

            self.assertTrue(child_gone, f"Child PID {child_pid} still alive")
            self.assertTrue(grandchild_gone, f"Grandchild PID {grandchild_pid} still alive")

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
