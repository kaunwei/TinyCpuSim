"""Firmware test execution harness with bounded execution and process cleanup."""

import os
import signal
import subprocess
import time
from typing import Callable, List, Optional


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


class FirmwareCancellationError(Exception):
    """Raised when a bounded process execution is cancelled."""

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


def run_bounded(
    command: List[str],
    timeout: float,
    *,
    cancelled: Optional[Callable[[], bool]] = None,
) -> subprocess.CompletedProcess[str]:
    """Execute a command bounded by timeout within its own process group.

    When cancelled callback is provided, bounded communicate polling is
    performed against a single monotonic deadline established at entry.
    During-execution cancellation or callback exception triggers cleanup of
    the owned process group and reaps the child process.
    Callbacks consume the execution deadline without resetting it.
    Cancellation interrupts the direct child with SIGINT for up to one second,
    then kills the original group and uses the existing bounded drain. Nested
    sessions require cooperative wrapper cleanup; arbitrary escapes are not killed.
    Callbacks must return promptly; this synchronous API cannot preempt a
    callback that never returns.

    Launches process group with start_new_session=True.
    On deadline, kills the process group, reaps the child process, and raises
    FirmwareTimeoutError containing captured stdout, stderr, and pid.
    """
    if cancelled is None:
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

    deadline = time.monotonic() + timeout

    if cancelled():
        raise FirmwareCancellationError(
            "Execution cancelled before process launch",
            stdout="",
            stderr="",
            pid=None,
        )

    if time.monotonic() >= deadline:
        raise FirmwareTimeoutError(
            f"Process timed out after {timeout} seconds",
            stdout="",
            stderr="",
            pid=None,
        )

    proc = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    pid = proc.pid

    latest_stdout = ""
    latest_stderr = ""
    cleaned_up = False

    def _normalize_stream(data: object) -> str:
        if data is None:
            return ""
        if isinstance(data, bytes):
            return data.decode("utf-8", errors="replace")
        return str(data)

    def _cleanup_and_drain(*, cooperative: bool = False) -> None:
        nonlocal cleaned_up, latest_stdout, latest_stderr
        if cleaned_up:
            return
        cleaned_up = True
        if cooperative:
            # Interrupt the direct wrapper so its own harness can clean nested sessions.
            try:
                proc.send_signal(signal.SIGINT)
            except ProcessLookupError:
                pass
            try:
                out, err = proc.communicate(timeout=1.0)
                if out is not None:
                    latest_stdout = _normalize_stream(out)
                if err is not None:
                    latest_stderr = _normalize_stream(err)
            except subprocess.TimeoutExpired as exc:
                if exc.output is not None:
                    latest_stdout = _normalize_stream(exc.output)
                if exc.stderr is not None:
                    latest_stderr = _normalize_stream(exc.stderr)
            except Exception:
                pass
        # Always clear the original owned group, even if its leader already exited.
        try:
            os.killpg(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            out, err = proc.communicate(timeout=2.0)
            if out is not None:
                latest_stdout = _normalize_stream(out)
            if err is not None:
                latest_stderr = _normalize_stream(err)
        except subprocess.TimeoutExpired as exc:
            if exc.output is not None:
                latest_stdout = _normalize_stream(exc.output)
            if exc.stderr is not None:
                latest_stderr = _normalize_stream(exc.stderr)
            try:
                if proc.stdout and not proc.stdout.closed:
                    proc.stdout.close()
            except Exception:
                pass
            try:
                if proc.stderr and not proc.stderr.closed:
                    proc.stderr.close()
            except Exception:
                pass
            try:
                proc.kill()
                proc.wait(timeout=2.0)
            except Exception:
                pass
        except Exception:
            try:
                if proc.stdout and not proc.stdout.closed:
                    proc.stdout.close()
            except Exception:
                pass
            try:
                if proc.stderr and not proc.stderr.closed:
                    proc.stderr.close()
            except Exception:
                pass
            try:
                proc.kill()
                proc.wait(timeout=2.0)
            except Exception:
                pass

    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                _cleanup_and_drain()
                raise FirmwareTimeoutError(
                    f"Process {pid} timed out after {timeout} seconds",
                    stdout=latest_stdout,
                    stderr=latest_stderr,
                    pid=pid,
                )

            poll_timeout = min(0.05, remaining)
            try:
                out, err = proc.communicate(timeout=poll_timeout)
                if out is not None:
                    latest_stdout = _normalize_stream(out)
                if err is not None:
                    latest_stderr = _normalize_stream(err)
                try:
                    os.killpg(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()
                cleaned_up = True
                return subprocess.CompletedProcess(
                    args=command,
                    returncode=proc.returncode,
                    stdout=latest_stdout,
                    stderr=latest_stderr,
                )
            except subprocess.TimeoutExpired as exc:
                if exc.output is not None:
                    latest_stdout = _normalize_stream(exc.output)
                if exc.stderr is not None:
                    latest_stderr = _normalize_stream(exc.stderr)

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                _cleanup_and_drain()
                raise FirmwareTimeoutError(
                    f"Process {pid} timed out after {timeout} seconds",
                    stdout=latest_stdout,
                    stderr=latest_stderr,
                    pid=pid,
                )

            try:
                is_cancelled = cancelled()
            except BaseException as cb_exc:
                _cleanup_and_drain(cooperative=True)
                for attr, val in (("stdout", latest_stdout), ("stderr", latest_stderr), ("pid", pid)):
                    try:
                        if not hasattr(cb_exc, attr):
                            setattr(cb_exc, attr, val)
                    except Exception:
                        pass
                raise

            if time.monotonic() >= deadline:
                _cleanup_and_drain()
                raise FirmwareTimeoutError(
                    f"Process {pid} timed out after {timeout} seconds",
                    stdout=latest_stdout,
                    stderr=latest_stderr,
                    pid=pid,
                )

            if is_cancelled:
                _cleanup_and_drain(cooperative=True)
                raise FirmwareCancellationError(
                    f"Process {pid} cancelled during execution",
                    stdout=latest_stdout,
                    stderr=latest_stderr,
                    pid=pid,
                )
    except (FirmwareTimeoutError, FirmwareCancellationError):
        raise
    except BaseException:
        _cleanup_and_drain()
        raise
