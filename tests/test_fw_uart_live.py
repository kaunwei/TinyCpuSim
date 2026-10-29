"""Tests for live firmware UART streaming and signal guard integration."""

import fcntl
import os
import pathlib
import signal
import struct
import tempfile
import time
import unittest
import subprocess

from scripts.firmware_test_harness import run_bounded
from tests.fw_control_test_support import controlled_firmware

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


def _create_uart_elf(elf_template_path: pathlib.Path, output_path: pathlib.Path, infinite_loop: bool = False) -> None:
    image = bytearray(elf_template_path.read_bytes())
    program_offset = struct.unpack_from("<I", image, 28)[0]
    program_entry_size = struct.unpack_from("<H", image, 42)[0]
    program_count = struct.unpack_from("<H", image, 44)[0]
    reset_address = 0x00010080
    reset_file_offset = None
    for index in range(program_count):
        header = program_offset + index * program_entry_size
        kind, offset, _vaddr, paddr, file_size = struct.unpack_from("<IIIII", image, header)
        if kind == 1 and paddr <= reset_address and reset_address + 2 <= paddr + file_size:
            reset_file_offset = offset + reset_address - paddr
            break
    if reset_file_offset is None:
        raise RuntimeError("fixture must contain reset handler")

    # LDR r0, =0x40000000; MOV r1, #'O'; STR r1, [r0]; MOV r1, #'K'; STR r1, [r0]
    # then if infinite_loop: B . (0xE7FE)
    # else: MOV r0, #0; SVC #0; NOP
    if infinite_loop:
        instructions = (0x4803, 0x214F, 0x6001, 0x214B, 0x6001, 0xE7FE, 0xBF00, 0xBF00)
    else:
        instructions = (0x4803, 0x214F, 0x6001, 0x214B, 0x6001, 0x2000, 0xDF00, 0xBF00)

    for index, halfword in enumerate(instructions):
        struct.pack_into("<H", image, reset_file_offset + index * 2, halfword)
    struct.pack_into("<I", image, reset_file_offset + 0x10, 0x40000000)
    output_path.write_bytes(image)



def _create_echo_elf(elf_template_path: pathlib.Path, output_path: pathlib.Path) -> None:
    image = bytearray(elf_template_path.read_bytes())
    program_offset = struct.unpack_from("<I", image, 28)[0]
    program_entry_size = struct.unpack_from("<H", image, 42)[0]
    program_count = struct.unpack_from("<H", image, 44)[0]
    reset_address = 0x00010080
    reset_file_offset = None
    for index in range(program_count):
        header = program_offset + index * program_entry_size
        kind, offset, _vaddr, paddr, file_size = struct.unpack_from("<IIIII", image, header)
        if kind == 1 and paddr <= reset_address and reset_address + 2 <= paddr + file_size:
            reset_file_offset = offset + reset_address - paddr
            break
    if reset_file_offset is None:
        raise RuntimeError("fixture must contain reset handler")

    # Thumb-16 echo loop:
    # 0x00: ldr r0, =0x40000000 (literal at offset 0x14) -> 0x4804
    # 0x02: ldr r1, [r0, #4]      ; poll STATUS -> 0x6841
    # 0x04: movs r2, #2           ; RX_VALID bit -> 0x2202
    # 0x06: tst r1, r2            -> 0x4211
    # 0x08: beq 0x02              ; if not valid, loop -> 0xD0FB
    # 0x0A: ldrb r3, [r0, #0]     ; read DATA byte -> 0x7803
    # 0x0C: strb r3, [r0, #0]     ; write DATA byte (echo) -> 0x7003
    # 0x0E: b 0x02                ; loop back to poll -> 0xE7F8
    # 0x10: nop                   -> 0xBF00
    # 0x12: nop                   -> 0xBF00
    instructions = (0x4804, 0x6841, 0x2202, 0x4211, 0xD0FB, 0x7803, 0x7003, 0xE7F8, 0xBF00, 0xBF00)
    for index, halfword in enumerate(instructions):
        struct.pack_into("<H", image, reset_file_offset + index * 2, halfword)
    struct.pack_into("<I", image, reset_file_offset + 0x14, 0x40000000)
    output_path.write_bytes(image)

class TestFwUartLive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sim = _REPO_ROOT / "build" / "tinycpusim"
        cls.template = _REPO_ROOT / "build" / "isa_boot_acceptance.elf"
        if not cls.sim.is_file() or not cls.template.is_file():
            raise unittest.SkipTest("Simulator or acceptance ELF not built")

    def test_live_uart_arrives_while_paused_and_no_replay_on_shutdown(self):
        with tempfile.TemporaryDirectory(prefix="fw-live-") as tmpdir:
            elf = pathlib.Path(tmpdir) / "loop.elf"
            _create_uart_elf(self.template, elf, infinite_loop=True)

            # Breakpoint at loop instruction 0x0001008A
            initial_cmds = "break 0x0001008a\n"
            with controlled_firmware(initial_commands=initial_cmds, elf_path=elf, host_timeout=5.0) as session:
                msg_set = session.receive()
                self.assertEqual(msg_set.get("event"), "breakpoint_set")
                msg_hit = session.receive()
                self.assertEqual(msg_hit.get("event"), "breakpoint")
                self.assertEqual(msg_hit.get("pc"), 0x0001008A)

                fd = session.proc.stdout.fileno()
                fl = fcntl.fcntl(fd, fcntl.F_GETFL)
                fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)

                time.sleep(0.05)
                received = session.proc.stdout.read()
                self.assertEqual(received, "OK")

                fcntl.fcntl(fd, fcntl.F_SETFL, fl)

                session.send("stop\n")
                msg_stopped = session.receive()
                self.assertEqual(msg_stopped.get("event"), "stopped")

                session.proc.wait(timeout=3.0)
                self.assertEqual(session.proc.returncode, 0)
                rest = session.proc.stdout.read()
                self.assertEqual(rest, "")

    def test_noncontrol_firmware_live_tx(self):
        with tempfile.TemporaryDirectory(prefix="fw-live-") as tmpdir:
            elf = pathlib.Path(tmpdir) / "normal.elf"
            _create_uart_elf(self.template, elf, infinite_loop=False)

            result = run_bounded(
                [str(self.sim), "--firmware", "--quiet", "--max-steps", "32", str(elf)],
                timeout=5.0,
            )
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "OK")

    def test_closed_stdout_pipe_quiet_epipe(self):
        with tempfile.TemporaryDirectory(prefix="fw-live-") as tmpdir:
            elf = pathlib.Path(tmpdir) / "normal.elf"
            _create_uart_elf(self.template, elf, infinite_loop=False)

            r, w = os.pipe()
            os.close(r)
            import subprocess
            proc = subprocess.Popen(
                [str(self.sim), "--firmware", "--quiet", "--max-steps", "32", str(elf)],
                stdout=w,
                stderr=subprocess.PIPE,
                text=True,
            )
            os.close(w)
            _, stderr = proc.communicate(timeout=5.0)
            self.assertNotEqual(proc.returncode, 0)
            self.assertNotEqual(proc.returncode, -signal.SIGPIPE)

    def test_signal_while_paused_restores_flags_and_exits_bounded(self):
        with tempfile.TemporaryDirectory(prefix="fw-live-") as tmpdir:
            elf = pathlib.Path(tmpdir) / "loop.elf"
            _create_uart_elf(self.template, elf, infinite_loop=True)

            with controlled_firmware(initial_commands="", elf_path=elf, host_timeout=5.0) as session:
                time.sleep(0.05)
                session.proc.send_signal(signal.SIGINT)
                session.proc.wait(timeout=3.0)
                self.assertEqual(session.proc.returncode, 128 + signal.SIGINT)

    def test_explicit_max_steps_zero_fails_immediately(self):
        with tempfile.TemporaryDirectory(prefix="fw-live-") as tmpdir:
            elf = pathlib.Path(tmpdir) / "normal.elf"
            _create_uart_elf(self.template, elf, infinite_loop=False)

            result = run_bounded(
                [str(self.sim), "--firmware", "--quiet", "--max-steps", "0", str(elf)],
                timeout=3.0,
            )
            self.assertNotEqual(result.returncode, 0)

    def test_uart_fd_bidirectional_echo(self):
        import socket
        with tempfile.TemporaryDirectory(prefix="fw-echo-") as tmpdir:
            elf = pathlib.Path(tmpdir) / "echo.elf"
            _create_echo_elf(self.template, elf)
            s_guest, s_host = socket.socketpair()
            s_host.setblocking(False)
            try:
                cmd = [str(self.sim), "--firmware", "--uart-fd", str(s_guest.fileno()), str(elf)]
                proc = subprocess.Popen(cmd, pass_fds=[s_guest.fileno()], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                s_guest.close()
                try:
                    s_host.sendall(b"PING1234")
                    received = bytearray()
                    start = time.monotonic()
                    while len(received) < 8 and time.monotonic() - start < 3.0:
                        try:
                            chunk = s_host.recv(64)
                            if chunk:
                                received.extend(chunk)
                        except (BlockingIOError, InterruptedError):
                            pass
                        time.sleep(0.01)
                    self.assertEqual(bytes(received), b"PING1234")
                finally:
                    proc.terminate()
                    proc.wait(timeout=2.0)
                    if proc.stdout:
                        proc.stdout.close()
                    if proc.stderr:
                        proc.stderr.close()
            finally:
                s_host.close()

    def test_uart_fd_pause_isolation_and_resume(self):
        import socket
        with tempfile.TemporaryDirectory(prefix="fw-echo-pause-") as tmpdir:
            elf = pathlib.Path(tmpdir) / "echo.elf"
            _create_echo_elf(self.template, elf)
            s_guest, s_host = socket.socketpair()
            ctrl_guest, ctrl_host = socket.socketpair()
            s_host.setblocking(False)
            ctrl_host.settimeout(3.0)
            try:
                cmd = [
                    str(self.sim),
                    "--firmware",
                    "--control-fd",
                    str(ctrl_guest.fileno()),
                    "--uart-fd",
                    str(s_guest.fileno()),
                    str(elf),
                ]
                proc = subprocess.Popen(
                    cmd,
                    pass_fds=[ctrl_guest.fileno(), s_guest.fileno()],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                ctrl_guest.close()
                s_guest.close()
                ctrl_file = ctrl_host.makefile("r", encoding="utf-8")
                try:
                    # 1. Send data while running, check echo
                    s_host.sendall(b"A")
                    start = time.monotonic()
                    echo_a = bytearray()
                    while not echo_a and time.monotonic() - start < 3.0:
                        try:
                            chunk = s_host.recv(16)
                            if chunk:
                                echo_a.extend(chunk)
                        except (BlockingIOError, InterruptedError):
                            pass
                        time.sleep(0.01)
                    self.assertEqual(bytes(echo_a), b"A")

                    # 2. Pause simulation
                    ctrl_host.sendall(b"pause\n")
                    line = ctrl_file.readline()
                    self.assertIn('"paused"', line)

                    # 3. Send data while paused: must NOT be echoed while paused
                    s_host.sendall(b"B")
                    time.sleep(0.1)
                    try:
                        echo_b = s_host.recv(16)
                    except BlockingIOError:
                        echo_b = b""
                    self.assertEqual(echo_b, b"", "Data must not be echoed while paused")

                    # 4. Resume: now the data must be echoed
                    ctrl_host.sendall(b"continue\n")
                    line = ctrl_file.readline()
                    self.assertIn('"continued"', line)

                    start = time.monotonic()
                    received = bytearray()
                    while not received and time.monotonic() - start < 3.0:
                        try:
                            chunk = s_host.recv(16)
                            if chunk:
                                received.extend(chunk)
                        except (BlockingIOError, InterruptedError):
                            pass
                        time.sleep(0.01)
                    self.assertEqual(bytes(received), b"B")

                    ctrl_host.sendall(b"stop\n")
                    line = ctrl_file.readline()
                    self.assertIn('"stopped"', line)
                    proc.wait(timeout=2.0)
                    self.assertEqual(proc.returncode, 0)
                finally:
                    ctrl_file.close()
                    if proc.poll() is None:
                        proc.terminate()
                    proc.wait(timeout=2.0)
                    if proc.stdout:
                        proc.stdout.close()
                    if proc.stderr:
                        proc.stderr.close()
            finally:
                ctrl_host.close()
                s_host.close()



if __name__ == "__main__":
    unittest.main()
