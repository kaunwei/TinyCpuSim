import subprocess
import unittest
from pathlib import Path


class TestFwQuietErrors(unittest.TestCase):
    def setUp(self):
        self.project_root = Path(__file__).resolve().parents[1]
        self.sim_binary = self.project_root / "build" / "tinycpusim"
        self.elf_binary = self.project_root / "build" / "isa_boot_acceptance.elf"
        self.malformed_elf = self.project_root / "CMakeLists.txt"
        self.assertTrue(self.sim_binary.is_file(), f"Binary missing: {self.sim_binary}")
        self.assertTrue(self.elf_binary.is_file(), f"ELF missing: {self.elf_binary}")
        self.assertTrue(self.malformed_elf.is_file(), f"Malformed ELF missing: {self.malformed_elf}")

    def test_quiet_maxsteps1_exit1_stdout_empty_stderr_diagnostic(self):
        result = subprocess.run(
            [
                str(self.sim_binary),
                "--firmware",
                "--max-steps",
                "1",
                "--quiet",
                str(self.elf_binary),
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )

        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("Simulation execution exceeded max step limit (1)", result.stderr)

    def test_quiet_malformed_elf_exit1_stdout_empty_stderr_load_error(self):
        result = subprocess.run(
            [
                str(self.sim_binary),
                "--firmware",
                "--quiet",
                str(self.malformed_elf),
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )

        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("Error loading ELF:", result.stderr)

    def test_quiet_plus_log_suppresses_cpu_trace(self):
        result = subprocess.run(
            [
                str(self.sim_binary),
                "--firmware",
                "--max-steps",
                "1",
                "--quiet",
                "--log",
                str(self.elf_binary),
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )

        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("Simulation execution exceeded max step limit (1)", result.stderr)
        self.assertNotIn("[trace]", result.stdout)
        self.assertNotIn("[trace]", result.stderr)
        self.assertNotIn("Starting simulation...", result.stdout)
        self.assertNotIn("Starting simulation...", result.stderr)

    def test_default_mode_unaltered_on_maxsteps1(self):
        result = subprocess.run(
            [
                str(self.sim_binary),
                "--firmware",
                "--max-steps",
                "1",
                str(self.elf_binary),
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )

        self.assertEqual(result.returncode, 1)
        self.assertIn("SIMULATION FAILED", result.stdout)
        self.assertIn("Simulation execution exceeded max step limit (1)", result.stdout)

    def test_successful_quiet_run_produces_empty_output(self):
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


if __name__ == "__main__":
    unittest.main()
