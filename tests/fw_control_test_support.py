import json
import pathlib
import socket
import subprocess
from contextlib import contextmanager

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


class FirmwareSession:
    def __init__(self, sock: socket.socket, proc: subprocess.Popen):
        self.sock = sock
        self.proc = proc
        self.process = proc
        self._reader = sock.makefile("r", encoding="utf-8")

    def send(self, data: str) -> None:
        self.sock.sendall(data.encode("utf-8"))

    def receive(self) -> dict:
        line = self._reader.readline()
        return json.loads(line) if line else {}

    def communicate(self, timeout: float | None = None):
        return self.proc.communicate(timeout=timeout)

    def close(self) -> None:
        self._reader.close()


@contextmanager
def controlled_firmware(initial_commands: str = "stop\n", max_steps: int | None = None):
    sim = str(_REPO_ROOT / "build" / "tinycpusim")
    elf = str(_REPO_ROOT / "build" / "isa_boot_acceptance.elf")
    parent_sock, child_sock = socket.socketpair()
    parent_sock.settimeout(2.0)
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
            cmd, pass_fds=(child_sock.fileno(),), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        child_sock.close()
        child_sock = None
        session = FirmwareSession(parent_sock, proc)
        yield session
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
