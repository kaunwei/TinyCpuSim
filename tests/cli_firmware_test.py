#!/usr/bin/env python3
"""Public CLI integration coverage for Cortex-M firmware boot."""

import subprocess
import struct
import sys
import tempfile
import unittest
from pathlib import Path


class FirmwareCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.simulator, cls.firmware_elf, cls.generic_elf = sys.argv[1:4]

    def run_simulator(self, *args):
        return subprocess.run(
            [self.simulator, *args],
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )

    @staticmethod
    def make_svc_elf(path, value, firmware):
        """Write a tiny independent ELF containing MOVW/MOVT R0 and SVC #0."""
        code = bytearray()
        low = value & 0xFFFF
        high = (value >> 16) & 0xFFFF
        for opcode, immediate in ((0xF240, low), (0xF2C0, high)):
            first = opcode | (((immediate >> 11) & 1) << 10) | (immediate >> 12)
            second = (((immediate >> 8) & 7) << 12) | (immediate & 0xFF)
            code.extend(struct.pack("<HH", first, second))
        code.extend(struct.pack("<H", 0xDF00))

        if firmware:
            payload = struct.pack("<II", 0x00030000, 0x00010009) + code
            entry = 0x00010000
        else:
            payload = code
            entry = 0x00010001

        ident = b"\x7fELF" + bytes((1, 1, 1, 0, 0)) + bytes(7)
        header = struct.pack(
            "<16sHHIIIIIHHHHHH", ident, 2, 40, 1, entry, 52, 0, 0,
            52, 32, 1, 0, 0, 0,
        )
        segment = struct.pack(
            "<IIIIIIII", 1, 0x100, 0x00010000, 0x00010000,
            len(payload), len(payload), 5, 0x100,
        )
        path.write_bytes(header + segment + bytes(0x100 - len(header) - len(segment)) + payload)

    def test_guest_svc_status_propagates_in_both_cli_modes(self):
        expected_host_status = {0: 0, 1: 1, 255: 255, 256: 1, 0x80000000: 1}
        with tempfile.TemporaryDirectory() as directory:
            for firmware in (False, True):
                for value, host_status in expected_host_status.items():
                    with self.subTest(firmware=firmware, value=value):
                        elf = Path(directory) / f"guest_{int(firmware)}_{value}.elf"
                        self.make_svc_elf(elf, value, firmware)
                        args = ("--firmware", str(elf)) if firmware else (str(elf),)
                        result = self.run_simulator(*args)
                        self.assertEqual(result.returncode, host_status, result.stdout + result.stderr)
                        self.assertIn(f"Exit Code:          {value} (R0 = {value})", result.stdout)

    def test_firmware_reset_boot_and_svc_exit(self):
        result = self.run_simulator("--firmware", self.firmware_elf)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SIMULATION PASSED", result.stdout)
        self.assertIn("Exit Code:          0 (R0 = 0)", result.stdout)

    def test_firmware_max_step_limit_is_reported_as_failure(self):
        result = self.run_simulator("--firmware", "--max-steps", "1", self.firmware_elf)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SIMULATION FAILED", result.stdout)
        self.assertIn("Simulation execution exceeded max step limit (1)", result.stdout)

    def test_firmware_cpu_fault_is_distinct_from_max_step_limit(self):
        image = bytearray(Path(self.firmware_elf).read_bytes())
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
        self.assertIsNotNone(reset_file_offset, "fixture must contain its reset handler")
        struct.pack_into("<H", image, reset_file_offset, 0xDE00)

        with tempfile.TemporaryDirectory() as directory:
            faulting_firmware = Path(directory) / "faulting_firmware.elf"
            faulting_firmware.write_bytes(image)
            result = self.run_simulator("--firmware", str(faulting_firmware))

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SIMULATION FAILED", result.stdout)
        self.assertIn("Undefined instruction 0x0000DE00", result.stdout)
        self.assertNotIn("max step limit", result.stdout)

    def test_generic_elf_mode_remains_available(self):
        result = self.run_simulator(self.generic_elf)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SIMULATION PASSED", result.stdout)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
