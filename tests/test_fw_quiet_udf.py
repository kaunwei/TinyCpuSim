"""Regression coverage for quiet firmware CPU faults."""

import struct
import tempfile
import unittest
from pathlib import Path

from scripts.firmware_test_harness import run_bounded


class TestFwQuietUndefinedInstruction(unittest.TestCase):
    def test_udf_reports_cpu_fault_on_stderr_without_stdout(self):
        repo_root = Path(__file__).resolve().parent.parent
        simulator = repo_root / "build" / "tinycpusim"
        firmware = repo_root / "build" / "isa_boot_acceptance.elf"
        self.assertTrue(simulator.is_file(), f"Simulator binary not found at {simulator}")
        self.assertTrue(firmware.is_file(), f"Firmware ELF not found at {firmware}")

        image = bytearray(firmware.read_bytes())
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

        with tempfile.TemporaryDirectory(prefix="quiet-udf-") as directory:
            faulting_firmware = Path(directory) / "faulting_firmware.elf"
            faulting_firmware.write_bytes(image)
            result = run_bounded(
                [
                    str(simulator),
                    "--firmware",
                    "--quiet",
                    "--max-steps",
                    "8",
                    str(faulting_firmware),
                ],
                timeout=5,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Undefined instruction 0x0000DE00", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
