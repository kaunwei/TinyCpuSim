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


    def test_postlaunch_callback_true_after_deadline_reports_timeout(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            ready = Path(tmpdir) / "ready"
            code = (
                "import os,sys,time;from pathlib import Path;"
                "print('DEADLINE_STDOUT',flush=True);"
                "print('DEADLINE_STDERR',file=sys.stderr,flush=True);"
                "Path(sys.argv[1]).write_text(str(os.getpid()));time.sleep(5)"
            )
            calls = []
            def cancelled() -> bool:
                calls.append(time.monotonic())
                if ready.exists():
                    time.sleep(0.30)
                    return True
                return False
            started = time.monotonic()
            try:
                with self.assertRaises(FirmwareTimeoutError) as ctx:
                    run_bounded([sys.executable, "-c", code, str(ready)],
                                timeout=0.15, cancelled=cancelled)
                error = ctx.exception
                pid = int(ready.read_text())
                self.assertEqual(error.pid, pid)
                self.assertEqual(error.stdout.count('DEADLINE_STDOUT\n'), 1)
                self.assertEqual(error.stderr.count('DEADLINE_STDERR\n'), 1)
                self.assertGreaterEqual(len(calls), 2)
                self.assertLess(time.monotonic() - started, 1.5)
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
            finally:
                if ready.exists():
                    try:
                        os.killpg(int(ready.read_text()), signal.SIGKILL)
                    except ProcessLookupError:
                        pass


    def test_callback_exception_unreadable_diagnostic_preserves_identity(self) -> None:
        class OriginalError(RuntimeError):
            @property
            def stdout(self):
                raise ValueError("diagnostic attribute cannot be read")
        sentinel = OriginalError("original callback failure")
        sentinel.stderr = "PREEXISTING_STDERR"
        with tempfile.TemporaryDirectory() as tmpdir:
            ready = Path(tmpdir) / "ready"
            code = (
                "import os,sys,time;from pathlib import Path;"
                "print('IDENTITY_STDOUT',flush=True);"
                "print('IDENTITY_STDERR',file=sys.stderr,flush=True);"
                "Path(sys.argv[1]).write_text(str(os.getpid()));time.sleep(5)"
            )
            def cancelled() -> bool:
                if ready.exists():
                    raise sentinel
                return False
            started = time.monotonic()
            try:
                with self.assertRaises(OriginalError) as ctx:
                    run_bounded([sys.executable, "-c", code, str(ready)],
                                timeout=1.0, cancelled=cancelled)
                self.assertIs(ctx.exception, sentinel)
                self.assertEqual(sentinel.stderr, "PREEXISTING_STDERR")
                pid = int(ready.read_text())
                self.assertEqual(sentinel.pid, pid)
                self.assertLess(time.monotonic() - started, 1.5)
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
            finally:
                if ready.exists():
                    try:
                        os.killpg(int(ready.read_text()), signal.SIGKILL)
                    except ProcessLookupError:
                        pass


@unittest.skipUnless(os.name == "posix" and Path("/proc").is_dir(), "Linux owned-process fixtures")
class FirmwareCooperativeCancellationTest(unittest.TestCase):
    def alive(self, pid):
        try:
            return Path(f"/proc/{pid}/stat").read_text().split()[2] != "Z"
        except FileNotFoundError:
            return False

    def cleanup_owned(self, paths):
        import json
        groups = set()
        for path in paths:
            if path.exists():
                try:
                    groups.update(json.loads(path.read_text())["groups"])
                except (ValueError, KeyError):
                    pass
        for group in groups:
            if group == os.getpgrp():
                continue
            try:
                os.killpg(group, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def assert_nonlive(self, pids):
        deadline = time.monotonic() + 0.7
        while any(self.alive(pid) for pid in pids) and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertFalse(any(self.alive(pid) for pid in pids), f"owned PIDs still live: {pids}")

    def exercise_nested(self, callback_exception):
        import json
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            outer_record, inner_record = root / "outer.json", root / "inner.json"
            ready, cleaned = root / "ready", root / "cleaned"
            repo = str(Path(__file__).resolve().parents[1])
            inner = (
                "import json,os,subprocess,sys,time;from pathlib import Path;"
                "child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)']);"
                + f"Path({str(inner_record)!r}).write_text(json.dumps({{'leader':os.getpid(),'child':child.pid,'groups':[os.getpgrp()]}}));"
                + f"Path({str(ready)!r}).touch();time.sleep(30)"
            )
            outer = (
                "import json,os,sys;from pathlib import Path\n"
                + f"sys.path.insert(0,{repo!r})\n"
                + "from scripts.firmware_test_harness import run_bounded\n"
                + f"Path({str(outer_record)!r}).write_text(json.dumps({{'leader':os.getpid(),'groups':[os.getpgrp()]}}))\n"
                + "print('NESTED_STDOUT',flush=True)\nprint('NESTED_STDERR',file=sys.stderr,flush=True)\n"
                + "try:\n"
                + f" run_bounded([sys.executable,'-c',{inner!r}],30)\n"
                + "finally:\n"
                + f" Path({str(cleaned)!r}).write_text('cooperatively cleaned')\n"
                + " print('WRAPPER_CLEANUP_DONE',flush=True)\n"
            )
            sentinel = RuntimeError("original nested callback failure")
            calls_after_ready = 0
            def cancelled():
                nonlocal calls_after_ready
                if ready.exists():
                    calls_after_ready += 1
                    if callback_exception:
                        raise sentinel
                    return True
                return False
            started = time.monotonic()
            try:
                expected = RuntimeError if callback_exception else FirmwareCancellationError
                with self.assertRaises(expected) as ctx:
                    run_bounded([sys.executable, "-c", outer], 3, cancelled=cancelled)
                error = ctx.exception
                if callback_exception:
                    self.assertIs(error, sentinel)
                outer_pids = json.loads(outer_record.read_text())
                inner_pids = json.loads(inner_record.read_text())
                self.assertEqual(error.pid, outer_pids["leader"])
                self.assertTrue(cleaned.exists(), "runtime SIGINT must allow wrapper finally before test cleanup")
                self.assertEqual(cleaned.read_text(), "cooperatively cleaned")
                self.assertEqual(error.stdout.count("NESTED_STDOUT\n"), 1)
                self.assertEqual(error.stderr.count("NESTED_STDERR\n"), 1)
                self.assertIn("WRAPPER_CLEANUP_DONE", error.stdout)
                self.assertEqual(calls_after_ready, 1)
                self.assert_nonlive([outer_pids["leader"], inner_pids["leader"], inner_pids["child"]])
                with self.assertRaises(ProcessLookupError):
                    os.kill(outer_pids["leader"], 0)
                elapsed = time.monotonic() - started
                print("NESTED_COOPERATIVE", callback_exception, round(elapsed, 3))
                self.assertLess(elapsed, 2.5)
            finally:
                # Observe and assert runtime cleanup BEFORE this failure-only safety net.
                self.cleanup_owned([outer_record, inner_record])

    def test_cooperative_nested_cancellation_cleans_inner_session(self):
        self.exercise_nested(callback_exception=False)

    def test_cooperative_nested_callback_exception_keeps_identity(self):
        self.exercise_nested(callback_exception=True)

    def exercise_stubborn(self, detached):
        import json
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record, ready = root / "pids.json", root / "ready"
            child = (
                "import json,os,signal,subprocess,sys,time;from pathlib import Path;"
                "signal.signal(signal.SIGINT,signal.SIG_IGN);"
                + f"desc=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)'],start_new_session={detached!r});"
                + "print('STUBBORN_STDOUT',flush=True);print('STUBBORN_STDERR',file=sys.stderr,flush=True);"
                + f"Path({str(record)!r}).write_text(json.dumps({{'leader':os.getpid(),'child':desc.pid,'groups':[os.getpgrp(),os.getpgid(desc.pid)]}}));"
                + f"Path({str(ready)!r}).touch();time.sleep(30)"
            )
            calls_after_ready = 0
            def cancelled():
                nonlocal calls_after_ready
                if ready.exists():
                    calls_after_ready += 1
                    return True
                return False
            started = time.monotonic()
            try:
                with self.assertRaises(FirmwareCancellationError) as ctx:
                    run_bounded([sys.executable, "-c", child], 5, cancelled=cancelled)
                elapsed = time.monotonic() - started
                pids = json.loads(record.read_text())
                error = ctx.exception
                self.assertEqual(error.pid, pids["leader"])
                self.assertEqual(error.stdout.count("STUBBORN_STDOUT\n"), 1)
                self.assertEqual(error.stderr.count("STUBBORN_STDERR\n"), 1)
                self.assertEqual(calls_after_ready, 1)
                with self.assertRaises(ProcessLookupError):
                    os.kill(pids["leader"], 0)
                if detached:
                    self.assertTrue(self.alive(pids["child"]), "escaped fixture remains separately owned by test")
                    self.assertGreaterEqual(elapsed, 2.8)
                else:
                    self.assert_nonlive([pids["child"]])
                    self.assertGreaterEqual(elapsed, 0.95)
                print("STUBBORN_ESCAPED", detached, round(elapsed, 3))
                self.assertLess(elapsed, 4.5)
            finally:
                self.cleanup_owned([record])
                if record.exists():
                    pids = json.loads(record.read_text())
                    self.assert_nonlive([pids["leader"], pids["child"]])

    def test_sigint_ignoring_child_escalates_and_cleans_original_group(self):
        self.exercise_stubborn(detached=False)

    def test_escaped_pipe_holder_has_finite_drain_without_escaped_kill_claim(self):
        self.exercise_stubborn(detached=True)


if __name__ == "__main__":
    unittest.main()
