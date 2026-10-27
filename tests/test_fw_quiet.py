import subprocess
import unittest
from pathlib import Path


class FirmwareQuietOutputTest(unittest.TestCase):
    def setUp(self):
        self.project_root = Path(__file__).resolve().parents[1]
        self.sim_binary = self.project_root / "build" / "tinycpusim"
        self.elf_binary = self.project_root / "build" / "isa_boot_acceptance.elf"
        self.assertTrue(self.sim_binary.is_file(), f"Binary missing: {self.sim_binary}")
        self.assertTrue(self.elf_binary.is_file(), f"ELF missing: {self.elf_binary}")

    def test_firmware_quiet_output_suppresses_cpu_output_on_success(self):
        result = subprocess.run(
            [
                str(self.sim_binary),
                "--firmware",
                "--max-steps",
                "100000",
                "--quiet",
                str(self.elf_binary),
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )

        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, "")

    def test_firmware_normal_invocation_contains_starting_simulation(self):
        result = subprocess.run(
            [
                str(self.sim_binary),
                "--firmware",
                "--max-steps",
                "100000",
                str(self.elf_binary),
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )

        self.assertEqual(result.returncode, 0)
        self.assertIn("Starting simulation...", result.stdout)


if __name__ == "__main__":
    unittest.main()
