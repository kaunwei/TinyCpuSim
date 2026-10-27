"""Firmware test execution harness with bounded execution and process cleanup."""

import os
import signal
import subprocess
from typing import List, Optional


class FirmwareTimeoutError(TimeoutError):
    """Raised when a bounded process execution exceeds its timeout deadline."""

    def __init__(
        self,
        message: str,
        stdout: str = "",
        stderr: str = "",
        pid: Optional[int] = None,
    ) -> None:
        super().__init__(message)
        self.stdout = stdout
        self.stderr = stderr
        self.pid = pid


def run_bounded(command: List[str], timeout: float) -> subprocess.CompletedProcess[str]:
    """Execute a command bounded by timeout within its own process group.

    Launches process group with start_new_session=True.
    On deadline, kills the process group, reaps the child process, and raises
    FirmwareTimeoutError containing captured stdout, stderr, and pid.
    """
    proc = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )

    pid = proc.pid
    try:
        stdout_data, stderr_data = proc.communicate(timeout=timeout)
        try:
            os.killpg(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()
        return subprocess.CompletedProcess(
            args=command,
            returncode=proc.returncode,
            stdout=stdout_data,
            stderr=stderr_data,
        )
    except subprocess.TimeoutExpired:
        try:
            os.killpg(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

        try:
            stdout_data, stderr_data = proc.communicate(timeout=2.0)
        except Exception:
            stdout_data, stderr_data = "", ""
            proc.kill()
            proc.wait()

        raise FirmwareTimeoutError(
            f"Process {pid} timed out after {timeout} seconds",
            stdout=stdout_data or "",
            stderr=stderr_data or "",
            pid=pid,
        )
    except BaseException:
        try:
            os.killpg(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            proc.kill()
            proc.wait(timeout=2.0)
        except Exception:
            pass
        raise
