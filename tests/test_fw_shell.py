"""Tests for firmware ring buffer, UART non-blocking RX, and UART command shell (Slice 20)."""

import pathlib
import socket
import subprocess
import time
import unittest

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


class TestFirmwareShell(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sim = _REPO_ROOT / "build" / "tinyarmsim"
        cls.elf = _REPO_ROOT / "build" / "shell_acceptance.elf"
        if not cls.sim.exists():
            raise unittest.SkipTest("tinyarmsim executable missing")
        if not cls.elf.exists():
            raise unittest.SkipTest("shell_acceptance.elf fixture missing")

    def test_shell_acceptance_standalone(self):
        cmd = [
            str(self.sim),
            "--firmware",
            "--max-steps",
            "500000",
            str(self.elf),
        ]
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(result.returncode, 0, f"Sim failed with stderr:\n{result.stderr}")
        self.assertIn("SHELL_START", result.stdout)
        self.assertIn("RING_BUFFER_PASS", result.stdout)
        self.assertIn("PONG", result.stdout)
        self.assertIn("CMDS: ping, help, status, compute <N>, selfcheck", result.stdout)
        self.assertIn("STATUS: OK", result.stdout)
        self.assertIn("SELFCHECK: PASS", result.stdout)
        self.assertIn("RESULT: 55", result.stdout)
        self.assertIn("RESULT: 385", result.stdout)
        self.assertIn("ERR: unknown cmd", result.stdout)
        self.assertIn("SHELL_ACCEPTANCE_PASS", result.stdout)

    def test_shell_interactive_uart_fd(self):
        s_guest, s_host = socket.socketpair()
        s_host.setblocking(False)
        try:
            # Send input commands before launching guest
            # Commands: ping, status, compute 7 (140), and an overflowing line (>63 chars)
            payload = b"ping\r\nstatus\r\ncompute 7\r\n" + (b"X" * 64) + b"\r\n"
            s_host.sendall(payload)

            cmd = [
                str(self.sim),
                "--firmware",
                "--uart-fd",
                str(s_guest.fileno()),
                "--max-steps",
                "500000",
                str(self.elf),
            ]
            proc = subprocess.Popen(
                cmd,
                pass_fds=[s_guest.fileno()],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            s_guest.close()
            try:
                received = bytearray()
                start = time.monotonic()
                while time.monotonic() - start < 3.0:
                    try:
                        chunk = s_host.recv(128)
                        if chunk:
                            received.extend(chunk)
                    except (BlockingIOError, InterruptedError):
                        pass
                    if b"SHELL_ACCEPTANCE_PASS" in received:
                        break
                    time.sleep(0.01)

                proc.wait(timeout=3.0)
                self.assertEqual(proc.returncode, 0)

                output_str = received.decode("utf-8", errors="replace")
                self.assertIn("PONG", output_str)
                self.assertIn("STATUS: OK", output_str)
                self.assertIn("RESULT: 140", output_str)
                self.assertIn("ERR: line overflow", output_str)
                self.assertIn("SHELL_ACCEPTANCE_PASS", output_str)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                if proc.stdout:
                    proc.stdout.close()
                if proc.stderr:
                    proc.stderr.close()
        finally:
            s_host.close()


if __name__ == "__main__":
    unittest.main()
