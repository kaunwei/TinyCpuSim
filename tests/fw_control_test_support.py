import json
import os
import pathlib
import socket
import subprocess
import sys
import threading
from contextlib import contextmanager

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


class FirmwareSession:
    def __init__(self, sock: socket.socket, proc: subprocess.Popen, host_timeout: float):
        self.sock = sock
        self.proc = proc
        self.process = proc
        self._reader = sock.makefile("r", encoding="utf-8")
        self._lock = threading.Lock()
        self._expired = threading.Event()
        self._closed = False
        self._deadline_timer = threading.Timer(host_timeout, self._expire)
        self._deadline_timer.daemon = True
        self._deadline_timer.start()

    def _expire(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._expired.set()
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.proc.kill()
            except ProcessLookupError:
                pass
            self.proc.wait()

    def _raise_if_expired(self) -> None:
        if self._expired.is_set():
            raise TimeoutError("firmware host deadline expired")

    def send(self, data: str) -> None:
        self._raise_if_expired()
        try:
            self.sock.sendall(data.encode("utf-8"))
        except OSError:
            self._raise_if_expired()
            raise

    def receive(self) -> dict:
        self._raise_if_expired()
        try:
            line = self._reader.readline()
        except OSError:
            self._raise_if_expired()
            raise
        self._raise_if_expired()
        return json.loads(line) if line else {}

    def communicate(self, timeout: float | None = None):
        self._raise_if_expired()
        try:
            result = self.proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            self._raise_if_expired()
            raise
        self._raise_if_expired()
        return result

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
            self._deadline_timer.cancel()
            self._reader.close()


@contextmanager
def controlled_firmware(
    initial_commands: str = "stop\n",
    max_steps: int | None = None,
    elf_path: str | pathlib.Path | None = None,
    host_timeout: float = 10.0,
    *,
    build_root: str | pathlib.Path | None = None,
    env: dict | None = None,
):
    if host_timeout <= 0:
        raise ValueError("host_timeout must be positive")

    if build_root is not None:
        root = pathlib.Path(build_root).resolve()
    elif "FW_TEST_BUILD_ROOT" in os.environ:
        root = pathlib.Path(os.environ["FW_TEST_BUILD_ROOT"]).resolve()
    else:
        root = _REPO_ROOT / "build"

    sim_path = root / "tinycpusim"
    if not sim_path.exists():
        raise FileNotFoundError(f"Selected firmware simulator binary not found: {sim_path}")
    sim = str(sim_path)

    elf = str(elf_path) if elf_path is not None else str(root / "isa_boot_acceptance.elf")
    parent_sock, child_sock = socket.socketpair()
    parent_sock.settimeout(host_timeout)
    proc = None
    session = None
    try:
        if initial_commands:
            parent_sock.sendall(initial_commands.encode("utf-8"))
        cmd = [sim, "--firmware", "--quiet", "--control-fd", str(child_sock.fileno())]
        if max_steps is not None:
            cmd.extend(["--max-steps", str(max_steps)])
        cmd.append(elf)
        proc = subprocess.Popen(
            cmd,
            pass_fds=(child_sock.fileno(),),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        child_sock.close()
        child_sock = None
        session = FirmwareSession(parent_sock, proc, host_timeout)
        yield session
        if sys.exc_info()[0] is None:
            session._raise_if_expired()
    finally:
        if child_sock is not None:
            child_sock.close()
        if session is not None:
            session.close()
        parent_sock.close()
        if proc is not None:
            if proc.poll() is None:
                proc.kill()
            proc.wait(timeout=2.0)
            if proc.stdout:
                proc.stdout.close()
            if proc.stderr:
                proc.stderr.close()
