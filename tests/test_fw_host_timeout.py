"""Tests for host timeout and process cleanup harness."""

import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from scripts.firmware_test_harness import (
    FirmwareCancellationError,
    FirmwareTimeoutError,
    run_bounded,
)


class FirmwareHostTimeoutTest(unittest.TestCase):
    def test_run_bounded_success(self) -> None:
        cmd = [sys.executable, "-c", 'print("ok")']
        res = run_bounded(cmd, timeout=2.0)
        self.assertEqual(res.returncode, 0)
        self.assertEqual(res.stdout, "ok\n")
        self.assertEqual(res.stderr, "")

    def test_run_bounded_cleans_descendants_after_direct_exit(self) -> None:
        for return_code in (0, 7):
            with self.subTest(return_code=return_code):
                child_code = (
                    "import subprocess, sys\n"
                    "sleeper = subprocess.Popen([sys.executable, '-c', "
                    "'import time; time.sleep(20)'], "
                    "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n"
                    "print(f'DESCENDANT:{sleeper.pid}', flush=True)\n"
                    f"sys.exit({return_code})\n"
                )
                result = run_bounded([sys.executable, "-c", child_code], timeout=2.0)
                self.assertEqual(result.returncode, return_code)
                self.assertEqual(result.stderr, "")
                self.assertTrue(result.stdout.startswith("DESCENDANT:"))
                descendant_pid = int(result.stdout.split(":", 1)[1])
                child_pid = descendant_pid  # The direct child owns the new session PGID.

                def is_pid_live(pid: int) -> bool:
                    try:
                        os.kill(pid, 0)
                    except ProcessLookupError:
                        return False
                    try:
                        with open(f"/proc/{pid}/status", "r") as status:
                            for line in status:
                                if line.startswith("State:"):
                                    return line.split()[1] != "Z"
                    except (FileNotFoundError, ProcessLookupError, PermissionError):
                        return False
                    return True

                try:
                    deadline = time.monotonic() + 2.0
                    while is_pid_live(descendant_pid) and time.monotonic() < deadline:
                        time.sleep(0.02)
                    self.assertFalse(
                        is_pid_live(descendant_pid),
                        f"Descendant PID {descendant_pid} still running after normal exit",
                    )
                finally:
                    # Every run_bounded child starts a new session with its PID as PGID.
                    try:
                        os.killpg(child_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

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

    def test_run_bounded_descendant_pipe_inheritance_timeout_and_cleanup(self) -> None:
        child_code = (
            "import os, subprocess, sys\n"
            "gc = subprocess.Popen(['sleep', '30'], close_fds=False)\n"
            "sys.stdout.write(f'READY:{os.getpid()}:{gc.pid}\\n')\n"
            "sys.stdout.flush()\n"
            "sys.exit(0)\n"
        )
        cmd = [sys.executable, "-c", child_code]

        with self.assertRaises(FirmwareTimeoutError) as ctx:
            run_bounded(cmd, timeout=0.3)

        err = ctx.exception
        self.assertIn("READY:", err.stdout)

        ready_line = [
            line for line in err.stdout.splitlines() if line.startswith("READY:")
        ][0]
        _, child_pid_str, gc_pid_str = ready_line.split(":")
        child_pid = int(child_pid_str)
        grandchild_pid = int(gc_pid_str)

        self.assertEqual(err.pid, child_pid)

        # Direct child was reaped by run_bounded and must be gone
        with self.assertRaises(ProcessLookupError):
            os.kill(child_pid, 0)

        # Bounded cleanup check that no living descendant remains in the process group.
        # On Linux, an orphaned non-child grandchild reaped by init may transiently exist
        # as a zombie (state 'Z') until init reaps it; distinguish 'Z' from live.
        def is_pid_live(pid: int) -> bool:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return False
            try:
                with open(f"/proc/{pid}/status", "r") as f:
                    for line in f:
                        if line.startswith("State:"):
                            return line.split()[1] != "Z"
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                return False
            return True

        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            if not is_pid_live(grandchild_pid):
                break
            time.sleep(0.02)

        self.assertFalse(
            is_pid_live(grandchild_pid),
            f"Descendant PID {grandchild_pid} still running after process group cleanup",
        )

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

    def test_run_bounded_preflight_cancelled_true_raises_typed_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            marker = Path(tmpdir) / "marker.txt"
            cmd = [
                sys.executable,
                "-c",
                f"from pathlib import Path; Path({str(marker)!r}).write_text('ran')",
            ]
            with self.assertRaises(FirmwareCancellationError) as ctx:
                run_bounded(cmd, timeout=2.0, cancelled=lambda: True)

            err = ctx.exception
            self.assertEqual(err.stdout, "")
            self.assertEqual(err.stderr, "")
            self.assertIsNone(err.pid)
            self.assertFalse(marker.exists(), "Command must not have run when cancelled before launch")

    def test_run_bounded_preflight_cancelled_exception_propagates_identical(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            marker = Path(tmpdir) / "marker.txt"
            cmd = [
                sys.executable,
                "-c",
                f"from pathlib import Path; Path({str(marker)!r}).write_text('ran')",
            ]
            sentinel_exc = RuntimeError("sentinel cancellation failure")

            def raising_cb() -> bool:
                raise sentinel_exc

            with self.assertRaises(RuntimeError) as ctx:
                run_bounded(cmd, timeout=2.0, cancelled=raising_cb)

            self.assertIs(ctx.exception, sentinel_exc)
            self.assertFalse(marker.exists(), "Command must not have run when callback raised before launch")

    def test_run_bounded_preflight_cancelled_false_executes_normally(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            marker = Path(tmpdir) / "marker.txt"
            cmd = [
                sys.executable,
                "-c",
                f"import sys; from pathlib import Path; Path({str(marker)!r}).write_text('ran'); "
                "sys.stdout.write('out_test\\n'); sys.stderr.write('err_test\\n'); sys.exit(7)",
            ]
            res = run_bounded(cmd, timeout=2.0, cancelled=lambda: False)

            self.assertEqual(res.returncode, 7)
            self.assertEqual(res.stdout, "out_test\n")
            self.assertEqual(res.stderr, "err_test\n")
            self.assertTrue(marker.exists(), "Command must execute when cancelled callback returns False")

    def test_run_bounded_cancellation_cleans_child_and_samegroup_descendant(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            ready_file = Path(tmpdir) / "ready.txt"
            pids_file = Path(tmpdir) / "pids.txt"

            child_code = f"""
import os
import subprocess
import sys
import time
from pathlib import Path

descendant = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
sys.stdout.write("unique_cancel_stdout\\n")
sys.stdout.flush()
sys.stderr.write("unique_cancel_stderr\\n")
sys.stderr.flush()

Path({repr(str(pids_file))}).write_text(f"{{os.getpid()}} {{descendant.pid}}")
Path({repr(str(ready_file))}).write_text("ready")
time.sleep(30)
"""
            child_pgid = None

            def is_pid_live(pid: int) -> bool:
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    return False
                try:
                    with open(f"/proc/{pid}/status", "r") as f:
                        for line in f:
                            if line.startswith("State:"):
                                return line.split()[1] != "Z"
                except (FileNotFoundError, ProcessLookupError, PermissionError):
                    return False
                return True

            def cancelled_cb() -> bool:
                return ready_file.exists() and pids_file.exists()

            cmd = [sys.executable, "-c", child_code]
            t0 = time.monotonic()
            try:
                with self.assertRaises(FirmwareCancellationError) as ctx:
                    run_bounded(cmd, timeout=10.0, cancelled=cancelled_cb)
                elapsed = time.monotonic() - t0
                err = ctx.exception

                self.assertIn("unique_cancel_stdout", err.stdout)
                self.assertIn("unique_cancel_stderr", err.stderr)
                self.assertIsNotNone(err.pid)

                raw_pids = pids_file.read_text().strip().split()
                child_pid = int(raw_pids[0])
                descendant_pid = int(raw_pids[1])
                child_pgid = child_pid

                self.assertEqual(err.pid, child_pid)

                # Direct child reaped
                with self.assertRaises(ProcessLookupError):
                    os.kill(child_pid, 0)

                # Descendant in same process group must no longer be live
                deadline = time.monotonic() + 3.0
                while time.monotonic() < deadline and is_pid_live(descendant_pid):
                    time.sleep(0.02)
                self.assertFalse(
                    is_pid_live(descendant_pid),
                    f"Descendant PID {descendant_pid} still running after cancellation cleanup",
                )
                self.assertLess(elapsed, 4.0, f"Elapsed time {elapsed}s exceeded bounded allowance")
            finally:
                if child_pgid is not None:
                    try:
                        os.killpg(child_pgid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_run_bounded_callback_exception_propagates_identity_and_cleans_group(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            ready_file = Path(tmpdir) / "ready.txt"
            pids_file = Path(tmpdir) / "pids.txt"

            child_code = f"""
import os
import subprocess
import sys
import time
from pathlib import Path

descendant = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
sys.stdout.write("out_before_cb_exc\\n")
sys.stdout.flush()
sys.stderr.write("err_before_cb_exc\\n")
sys.stderr.flush()

Path({repr(str(pids_file))}).write_text(f"{{os.getpid()}} {{descendant.pid}}")
Path({repr(str(ready_file))}).write_text("ready")
time.sleep(30)
"""
            child_pgid = None
            sentinel_exc = RuntimeError("sentinel callback exception after launch")

            def is_pid_live(pid: int) -> bool:
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    return False
                try:
                    with open(f"/proc/{pid}/status", "r") as f:
                        for line in f:
                            if line.startswith("State:"):
                                return line.split()[1] != "Z"
                except (FileNotFoundError, ProcessLookupError, PermissionError):
                    return False
                return True

            def raising_cb() -> bool:
                if ready_file.exists() and pids_file.exists():
                    raise sentinel_exc
                return False

            cmd = [sys.executable, "-c", child_code]
            t0 = time.monotonic()
            try:
                with self.assertRaises(RuntimeError) as ctx:
                    run_bounded(cmd, timeout=10.0, cancelled=raising_cb)
                elapsed = time.monotonic() - t0

                # Re-raised by identity
                self.assertIs(ctx.exception, sentinel_exc)

                # Diagnostics attached best-effort
                self.assertTrue(hasattr(sentinel_exc, "stdout"))
                self.assertIn("out_before_cb_exc", getattr(sentinel_exc, "stdout"))
                self.assertTrue(hasattr(sentinel_exc, "stderr"))
                self.assertIn("err_before_cb_exc", getattr(sentinel_exc, "stderr"))
                self.assertTrue(hasattr(sentinel_exc, "pid"))

                raw_pids = pids_file.read_text().strip().split()
                child_pid = int(raw_pids[0])
                descendant_pid = int(raw_pids[1])
                child_pgid = child_pid

                self.assertEqual(getattr(sentinel_exc, "pid"), child_pid)

                # Direct child reaped
                with self.assertRaises(ProcessLookupError):
                    os.kill(child_pid, 0)

                # Descendant in same group cleaned
                deadline = time.monotonic() + 3.0
                while time.monotonic() < deadline and is_pid_live(descendant_pid):
                    time.sleep(0.02)
                self.assertFalse(
                    is_pid_live(descendant_pid),
                    f"Descendant PID {descendant_pid} still running after callback exception cleanup",
                )
                self.assertLess(elapsed, 4.0)
            finally:
                if child_pgid is not None:
                    try:
                        os.killpg(child_pgid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_run_bounded_periodic_output_alwaysfalse_callback_reaches_original_deadline(self) -> None:
        child_code = (
            "import sys, time\n"
            "for i in range(25):\n"
            "    sys.stdout.write(f'CHUNK_{i}\\n')\n"
            "    sys.stdout.flush()\n"
            "    time.sleep(0.03)\n"
        )
        cmd = [sys.executable, "-c", child_code]
        t0 = time.monotonic()
        with self.assertRaises(FirmwareTimeoutError) as ctx:
            run_bounded(cmd, timeout=0.25, cancelled=lambda: False)
        elapsed = time.monotonic() - t0

        err = ctx.exception
        self.assertIsNotNone(err.pid)
        self.assertIn("CHUNK_0\n", err.stdout)
        # Repeated stdout prefix appears exactly once (communicate returns accumulated data)
        self.assertEqual(err.stdout.count("CHUNK_0\n"), 1)
        # Verify elapsed time respects original deadline without resetting (<1.5s)
        self.assertLess(elapsed, 1.5, f"Elapsed time {elapsed}s indicates deadline was reset")
        self.assertGreaterEqual(elapsed, 0.20, f"Elapsed time {elapsed}s exited too early")


if __name__ == "__main__":
    unittest.main()
