import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class FirmwareTemplateTest(unittest.TestCase):
    def _build_and_run_for_opt(self, project_root: Path, opt_flag: str):
        template_dir = project_root / "firmware" / "template"
        simulator = project_root / "build" / "tinycpusim"

        self.assertTrue(simulator.is_file(), f"Simulator binary not found at {simulator}")
        self.assertTrue(template_dir.is_dir(), f"Template directory not found at {template_dir}")

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_template = Path(temp_dir) / "template"
            shutil.copytree(template_dir, temp_template)

            # Build in isolated directory with specific OPT
            build_res = subprocess.run(
                ["make", f"OPT={opt_flag}"],
                cwd=temp_template,
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertEqual(
                build_res.returncode,
                0,
                f"Make build failed for {opt_flag}:\nSTDOUT:\n{build_res.stdout}\nSTDERR:\n{build_res.stderr}",
            )

            elf_path = temp_template / "template.elf"
            self.assertTrue(elf_path.is_file(), f"template.elf was not produced for {opt_flag}")

            # Check that main symbol exists via arm-none-eabi-nm (or nm fallback)
            nm_bin = shutil.which("arm-none-eabi-nm") or shutil.which("nm")
            self.assertIsNotNone(nm_bin, "Neither arm-none-eabi-nm nor nm found in PATH")

            nm_res = subprocess.run(
                [nm_bin, str(elf_path)],
                capture_output=True,
                text=True,
                timeout=5,
            )
            self.assertEqual(nm_res.returncode, 0, f"nm failed for {opt_flag}:\n{nm_res.stderr}")

            main_symbols = [
                line for line in nm_res.stdout.splitlines()
                if "main" in line.split() or line.rstrip().endswith(" main") or " T main" in line
            ]
            self.assertTrue(len(main_symbols) > 0, f"main symbol not found in nm output for {opt_flag}:\n{nm_res.stdout}")

            # Parse the main symbol address
            main_line = main_symbols[0]
            main_addr_str = main_line.split()[0]
            main_addr = int(main_addr_str, 16)
            main_addr_hex = f"0x{main_addr:08x}"

            # Execute under a small explicit step limit and host timeout, verifying trace reaches main
            sim_res = subprocess.run(
                [
                    str(simulator),
                    "--firmware",
                    "-l",
                    "--max-steps",
                    "60",
                    str(elf_path),
                ],
                capture_output=True,
                text=True,
                timeout=5,
            )

            combined_output = sim_res.stdout + sim_res.stderr
            # Ensure it didn't crash before running or fail due to ELF load error
            self.assertIn("Loading ELF:", combined_output)
            self.assertIn("Starting simulation...", combined_output)

            # Check that trace reaches main address
            self.assertIn(
                main_addr_hex,
                combined_output,
                f"Trace did not reach main at address {main_addr_hex} for {opt_flag}.\nOutput:\n{combined_output}",
            )

            # Check that execution exceeded step limit rather than hitting an unhandled fault/SVC error
            self.assertIn(
                "Simulation execution exceeded max step limit (60)",
                combined_output,
                f"Simulation did not hit expected max step limit for {opt_flag}.\nOutput:\n{combined_output}",
            )
            self.assertEqual(sim_res.returncode, 1)

    def test_standalone_copy_build_and_execute_o0(self):
        project_root = Path(__file__).resolve().parents[1]
        self._build_and_run_for_opt(project_root, "-O0")

    def test_standalone_copy_build_and_execute_o2(self):
        project_root = Path(__file__).resolve().parents[1]
        self._build_and_run_for_opt(project_root, "-O2")


if __name__ == "__main__":
    unittest.main()
