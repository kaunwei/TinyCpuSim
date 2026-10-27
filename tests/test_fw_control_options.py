import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwControlOptions(unittest.TestCase):
    def _run_cli(self, args_list):
        args_cpp = ", ".join(f'"{a}"' for a in args_list)
        source = f"""
#include <iostream>
#include <vector>
#include <string>
#include "tinyarmsim/cli_options.hpp"

int main() {{
    std::vector<std::string> args = {{{args_cpp}}};
    auto res = tinyarmsim::parse_cli_options(args);
    std::cout << "diag=" << (res.diagnostic.empty() ? "NONE" : "HAS_DIAG") << "\\n";
    std::cout << "control_fd=" << res.options.control_fd << "\\n";
    std::cout << "max_steps=" << res.options.max_steps << "\\n";
    std::cout << "max_steps_explicit=" << (res.options.max_steps_explicit ? "1" : "0") << "\\n";
    std::cout << "quiet=" << (res.options.quiet ? "1" : "0") << "\\n";
    std::cout << "enable_firmware=" << (res.options.enable_firmware ? "1" : "0") << "\\n";
    std::cout << "elf_path=" << res.options.elf_path << "\\n";
    return 0;
}}
"""
        res = run_cpp(source, timeout=10)
        self.assertEqual(res.returncode, 0, f"Compilation or run failed: {res.stderr}")
        kv = {}
        for line in res.stdout.strip().splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                kv[k] = v
        return kv

    def test_defaults(self):
        kv = self._run_cli(["app.elf"])
        self.assertEqual(kv.get("diag"), "NONE")
        self.assertEqual(kv.get("control_fd"), "-1")
        self.assertEqual(kv.get("max_steps_explicit"), "0")
        self.assertEqual(kv.get("max_steps"), "1000000000")
        self.assertEqual(kv.get("quiet"), "0")
        self.assertEqual(kv.get("enable_firmware"), "0")
        self.assertEqual(kv.get("elf_path"), "app.elf")

    def test_firmware_control_fd_separate_argv7_with_elf_path(self):
        kv = self._run_cli(["--firmware", "--control-fd", "7", "firmware.elf"])
        self.assertEqual(kv.get("diag"), "NONE")
        self.assertEqual(kv.get("control_fd"), "7")
        self.assertEqual(kv.get("enable_firmware"), "1")
        self.assertEqual(kv.get("elf_path"), "firmware.elf")
        self.assertEqual(kv.get("max_steps_explicit"), "0")

    def test_explicit_max_steps_2(self):
        # test both -m and --max-steps
        kv1 = self._run_cli(["-m", "2", "app.elf"])
        self.assertEqual(kv1.get("diag"), "NONE")
        self.assertEqual(kv1.get("max_steps"), "2")
        self.assertEqual(kv1.get("max_steps_explicit"), "1")

        kv2 = self._run_cli(["--max-steps", "2", "app.elf"])
        self.assertEqual(kv2.get("diag"), "NONE")
        self.assertEqual(kv2.get("max_steps"), "2")
        self.assertEqual(kv2.get("max_steps_explicit"), "1")

    def test_invalid_fd_inputs(self):
        # Missing argument
        kv_missing = self._run_cli(["--control-fd"])
        self.assertEqual(kv_missing.get("diag"), "HAS_DIAG")

        # Negative integer
        kv_neg = self._run_cli(["--control-fd", "-1", "app.elf"])
        self.assertEqual(kv_neg.get("diag"), "HAS_DIAG")

        # Non-integer / alphanumeric string
        kv_str = self._run_cli(["--control-fd", "abc", "app.elf"])
        self.assertEqual(kv_str.get("diag"), "HAS_DIAG")

        # Trailing garbage
        kv_trail = self._run_cli(["--control-fd", "7abc", "app.elf"])
        self.assertEqual(kv_trail.get("diag"), "HAS_DIAG")

        # Empty string argument
        kv_empty = self._run_cli(["--control-fd", "", "app.elf"])
        self.assertEqual(kv_empty.get("diag"), "HAS_DIAG")

        # Out-of-range descriptors above numeric_limits<int>::max()
        kv_int_max_plus_1 = self._run_cli(["--control-fd", "2147483648", "app.elf"])
        self.assertEqual(kv_int_max_plus_1.get("diag"), "HAS_DIAG")

        kv_wrap = self._run_cli(["--control-fd", "4294967296", "app.elf"])
        self.assertEqual(kv_wrap.get("diag"), "HAS_DIAG")

    def test_valid_fd_int_max(self):
        # Boundary: INT_MAX (2147483647) accepted at parser layer
        kv = self._run_cli(["--control-fd", "2147483647", "app.elf"])
        self.assertEqual(kv.get("diag"), "NONE")
        self.assertEqual(kv.get("control_fd"), "2147483647")

    def test_preserve_quiet_flag(self):
        kv = self._run_cli(["--quiet", "--firmware", "--control-fd", "3", "firmware.elf"])
        self.assertEqual(kv.get("diag"), "NONE")
        self.assertEqual(kv.get("quiet"), "1")
        self.assertEqual(kv.get("enable_firmware"), "1")
        self.assertEqual(kv.get("control_fd"), "3")
        self.assertEqual(kv.get("elf_path"), "firmware.elf")


if __name__ == "__main__":
    unittest.main()
