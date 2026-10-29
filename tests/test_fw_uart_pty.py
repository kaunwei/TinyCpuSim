"""Tests for firmware UART PTY host device and interactive terminal (Slices 11 & 12)."""

import fcntl
import os
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from tests.test_fw_uart_live import _create_echo_elf


class TestFirmwareUartPty(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sim = _REPO_ROOT / "build" / "tinyarmsim"
        cls.template = _REPO_ROOT / "build" / "isa_boot_acceptance.elf"
        if not cls.sim.exists() or not cls.template.exists():
            raise unittest.SkipTest("tinyarmsim executable or isa_boot_acceptance.elf fixture missing")

    def _wait_for_pty_path(self, pty_file: pathlib.Path, timeout: float = 3.0) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if pty_file.exists():
                try:
                    content = pty_file.read_text().strip()
                    if content and os.path.exists(content):
                        return content
                except OSError:
                    pass
            time.sleep(0.02)
        raise TimeoutError(f"PTY file {pty_file} not populated within {timeout}s")

    def test_pty_bidirectional_echo(self):
        with tempfile.TemporaryDirectory(prefix="fw-pty-echo-") as tmpdir:
            elf = pathlib.Path(tmpdir) / "echo.elf"
            pty_path_file = pathlib.Path(tmpdir) / "pty_slave.txt"
            _create_echo_elf(self.template, elf)

            cmd = [
                str(self.sim),
                "--firmware",
                "--uart-pty",
                "--pty-file",
                str(pty_path_file),
                str(elf),
            ]
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                slave_dev = self._wait_for_pty_path(pty_path_file)
                self.assertTrue(os.path.exists(slave_dev))

                slave_fd = os.open(slave_dev, os.O_RDWR | os.O_NOCTTY)
                try:
                    fcntl.fcntl(slave_fd, fcntl.F_SETFL, os.O_NONBLOCK)
                    test_payload = b"PING_FROM_PTY\n"
                    os.write(slave_fd, test_payload)

                    start = time.monotonic()
                    received = bytearray()
                    while len(received) < len(test_payload) and time.monotonic() - start < 3.0:
                        try:
                            chunk = os.read(slave_fd, 64)
                            if chunk:
                                received.extend(chunk)
                        except (BlockingIOError, InterruptedError):
                            pass
                        time.sleep(0.01)
                    self.assertEqual(bytes(received), test_payload)
                finally:
                    os.close(slave_fd)

                proc.send_signal(signal.SIGTERM)
                proc.wait(timeout=2.0)
                self.assertEqual(proc.returncode, 128 + signal.SIGTERM)
                self.assertFalse(pty_path_file.exists(), "pty file should be removed on clean teardown")
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                if proc.stdout:
                    proc.stdout.close()
                if proc.stderr:
                    proc.stderr.close()

    def test_pty_disconnect_and_reconnect(self):
        with tempfile.TemporaryDirectory(prefix="fw-pty-reconnect-") as tmpdir:
            elf = pathlib.Path(tmpdir) / "echo.elf"
            pty_path_file = pathlib.Path(tmpdir) / "pty_slave.txt"
            _create_echo_elf(self.template, elf)

            cmd = [
                str(self.sim),
                "--firmware",
                "--uart-pty",
                "--pty-file",
                str(pty_path_file),
                str(elf),
            ]
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                slave_dev = self._wait_for_pty_path(pty_path_file)

                # Client 1: connect, echo, disconnect
                fd1 = os.open(slave_dev, os.O_RDWR | os.O_NOCTTY)
                fcntl.fcntl(fd1, fcntl.F_SETFL, os.O_NONBLOCK)
                payload1 = b"SESSION_ONE"
                os.write(fd1, payload1)
                start = time.monotonic()
                received1 = bytearray()
                while len(received1) < len(payload1) and time.monotonic() - start < 3.0:
                    try:
                        chunk = os.read(fd1, 64)
                        if chunk:
                            received1.extend(chunk)
                    except (BlockingIOError, InterruptedError):
                        pass
                    time.sleep(0.01)
                self.assertEqual(bytes(received1), payload1)
                os.close(fd1)

                # While disconnected: wait and assert simulator stays healthy (no CPU peg, no crash)
                time.sleep(0.2)
                self.assertIsNone(proc.poll(), "Simulator must not crash on PTY disconnect")

                # Client 2: reconnect, echo, disconnect
                fd2 = os.open(slave_dev, os.O_RDWR | os.O_NOCTTY)
                fcntl.fcntl(fd2, fcntl.F_SETFL, os.O_NONBLOCK)
                payload2 = b"SESSION_TWO"
                os.write(fd2, payload2)
                start = time.monotonic()
                received2 = bytearray()
                while len(received2) < len(payload2) and time.monotonic() - start < 3.0:
                    try:
                        chunk = os.read(fd2, 64)
                        if chunk:
                            received2.extend(chunk)
                    except (BlockingIOError, InterruptedError):
                        pass
                    time.sleep(0.01)
                self.assertEqual(bytes(received2), payload2)
                os.close(fd2)

                proc.send_signal(signal.SIGTERM)
                proc.wait(timeout=2.0)
                self.assertEqual(proc.returncode, 128 + signal.SIGTERM)
                self.assertFalse(pty_path_file.exists(), "pty file should be removed on clean teardown")
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                if proc.stdout:
                    proc.stdout.close()
                if proc.stderr:
                    proc.stderr.close()

    def test_pty_pause_isolation_and_resume(self):
        with tempfile.TemporaryDirectory(prefix="fw-pty-pause-") as tmpdir:
            elf = pathlib.Path(tmpdir) / "echo.elf"
            pty_path_file = pathlib.Path(tmpdir) / "pty_slave.txt"
            _create_echo_elf(self.template, elf)

            ctrl_guest, ctrl_host = socket.socketpair()
            ctrl_host.settimeout(3.0)

            try:
                cmd = [
                    str(self.sim),
                    "--firmware",
                    "--control-fd",
                    str(ctrl_guest.fileno()),
                    "--uart-pty",
                    "--pty-file",
                    str(pty_path_file),
                    str(elf),
                ]
                proc = subprocess.Popen(
                    cmd,
                    pass_fds=[ctrl_guest.fileno()],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                ctrl_guest.close()
                ctrl_file = ctrl_host.makefile("r", encoding="utf-8")

                try:
                    slave_dev = self._wait_for_pty_path(pty_path_file)
                    slave_fd = os.open(slave_dev, os.O_RDWR | os.O_NOCTTY)
                    fcntl.fcntl(slave_fd, fcntl.F_SETFL, os.O_NONBLOCK)

                    try:
                        # 1. Echo while running
                        os.write(slave_fd, b"X")
                        start = time.monotonic()
                        echo_x = bytearray()
                        while not echo_x and time.monotonic() - start < 3.0:
                            try:
                                chunk = os.read(slave_fd, 16)
                                if chunk:
                                    echo_x.extend(chunk)
                            except (BlockingIOError, InterruptedError):
                                pass
                            time.sleep(0.01)
                        self.assertEqual(bytes(echo_x), b"X")

                        # 2. Pause simulation
                        ctrl_host.sendall(b"pause\n")
                        line = ctrl_file.readline()
                        self.assertIn('"paused"', line)

                        # 3. Write while paused: must NOT echo while paused
                        os.write(slave_fd, b"Y")
                        time.sleep(0.1)
                        try:
                            echo_y = os.read(slave_fd, 16)
                        except (BlockingIOError, InterruptedError):
                            echo_y = b""
                        self.assertEqual(echo_y, b"", "Data must not echo while paused")

                        # 4. Resume: now data should be echoed
                        ctrl_host.sendall(b"continue\n")
                        line = ctrl_file.readline()
                        self.assertIn('"continued"', line)

                        start = time.monotonic()
                        received = bytearray()
                        while not received and time.monotonic() - start < 3.0:
                            try:
                                chunk = os.read(slave_fd, 16)
                                if chunk:
                                    received.extend(chunk)
                            except (BlockingIOError, InterruptedError):
                                pass
                            time.sleep(0.01)
                        self.assertEqual(bytes(received), b"Y")

                        # 5. Stop cleanly
                        ctrl_host.sendall(b"stop\n")
                        line = ctrl_file.readline()
                        self.assertIn('"stopped"', line)
                        proc.wait(timeout=2.0)
                        self.assertEqual(proc.returncode, 0)
                    finally:
                        os.close(slave_fd)
                finally:
                    ctrl_file.close()
            finally:
                ctrl_host.close()
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                if proc.stdout:
                    proc.stdout.close()
                if proc.stderr:
                    proc.stderr.close()


if __name__ == "__main__":
    unittest.main()
