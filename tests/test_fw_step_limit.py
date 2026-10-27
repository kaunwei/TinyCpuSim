import subprocess
import unittest
from pathlib import Path


class FirmwareStepLimitTest(unittest.TestCase):
    def test_firmware_execution_exceeding_step_limit_exits_with_error(self):
        project_root = Path(__file__).resolve().parents[1]
        result = subprocess.run(
            [
                str(project_root / "build/tinycpusim"),
                "--firmware",
                "--max-steps",
                "1",
                str(project_root / "build/isa_boot_acceptance.elf"),
            ],
            capture_output=True,
            text=True,
            timeout=5,
        )

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Simulation execution exceeded max step limit (1)",
            result.stdout + result.stderr,
        )


if __name__ == "__main__":
    unittest.main()
