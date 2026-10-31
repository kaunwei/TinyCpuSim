"""Tests for firmware build matrix (-O0, -O2, Debug/Release) and integration acceptance (Slice 23)."""

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from scripts.firmware_test_harness import run_service_watchdog, FirmwareTimeoutError


class TestFirmwareBuildMatrix(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sim = _REPO_ROOT / "build" / "tinyarmsim"
        cls.ref_release = _REPO_ROOT / "build" / "reference_app.elf"
        cls.ref_debug = _REPO_ROOT / "build" / "reference_app_debug.elf"
        cls.template = _REPO_ROOT / "platforms" / "common" / "template"
        cls.gcc = shutil.which("arm-none-eabi-gcc")
        if not cls.sim.exists():
            raise unittest.SkipTest("tinyarmsim binary missing")
        if not cls.gcc:
            raise unittest.SkipTest("arm-none-eabi-gcc toolchain missing")

    def test_template_build_matrix_o0_and_o2(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            copied = pathlib.Path(tmpdir) / "template"
            shutil.copytree(self.template, copied)
            for opt in ["-O0", "-O2"]:
                res = subprocess.run(["make", f"OPT={opt}"], cwd=copied, capture_output=True, text=True)
                self.assertEqual(res.returncode, 0, f"make {opt} failed:\n{res.stderr}")
                elf = copied / "template.elf"
                self.assertTrue(elf.exists())
                probe = subprocess.run([str(self.sim), "--firmware", "--max-steps", "100", str(elf)],
                                       capture_output=True, text=True)
                self.assertEqual(probe.returncode, 1)

    def test_reference_app_release_and_debug_matrix(self):
        if not self.ref_release.exists() or not self.ref_debug.exists():
            raise unittest.SkipTest("reference_app ELFs missing")

        size_tool = shutil.which("arm-none-eabi-size") or shutil.which("size")
        if not size_tool:
            raise unittest.SkipTest("size utility missing")

        res = subprocess.run([size_tool, str(self.ref_release), str(self.ref_debug)],
                             capture_output=True, text=True)
        self.assertEqual(res.returncode, 0)
        lines = res.stdout.strip().splitlines()
        self.assertGreaterEqual(len(lines), 3)

        match_rel = re.search(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+.*reference_app\.elf", lines[1])
        match_dbg = re.search(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+.*reference_app_debug\.elf", lines[2])
        self.assertIsNotNone(match_rel)
        self.assertIsNotNone(match_dbg)

        bss_rel = int(match_rel.group(3))
        bss_dbg = int(match_dbg.group(3))
        self.assertEqual(bss_dbg - bss_rel, 48)

    def test_watchdog_no_progress_timeout(self):
        if not self.ref_release.exists():
            raise unittest.SkipTest("reference_app.elf missing")
        with self.assertRaises(FirmwareTimeoutError):
            run_service_watchdog(
                sim_binary=self.sim,
                elf_path=self.ref_release,
                target_service_predicate=lambda out: "UNSATISFIED_TARGET_STRING" in out,
                step_budget=50000,
                timeout_sec=1.5,
                heartbeat_token="[HB]",
            )


if __name__ == "__main__":
    unittest.main()
