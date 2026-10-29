"""Tests for live firmware UART streaming and signal guard integration."""

import fcntl
import os
import pathlib
import signal
import struct
import tempfile
import time
import unittest

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


if __name__ == "__main__":
    unittest.main()
