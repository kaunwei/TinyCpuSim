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
