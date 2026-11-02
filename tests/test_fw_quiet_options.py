import subprocess
import tempfile
from pathlib import Path
import unittest

class TestFwQuietOptions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        repo_root = Path(__file__).resolve().parent.parent
        cls.include_dir = repo_root / "include"
        cls.temp_dir = tempfile.TemporaryDirectory()
        cls.bin_path = Path(cls.temp_dir.name) / "parse_client"

        cpp_src = f"""#include <iostream>
#include <vector>
#include <string>
#include "tinyarmsim/utils/cli_options.hpp"

int main(int argc, char* argv[]) {{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {{
        args.emplace_back(argv[i]);
    }}
    auto res = tinyarmsim::parse_cli_options(args);
    std::cout << "quiet=" << (res.options.quiet ? "1" : "0") << "\\n";
    std::cout << "enable_firmware=" << (res.options.enable_firmware ? "1" : "0") << "\\n";
    std::cout << "elf_path=" << res.options.elf_path << "\\n";
    std::cout << "exit_code=" << res.exit_code << "\\n";
    return 0;
}}
"""
        src_path = Path(cls.temp_dir.name) / "parse_client.cpp"
        src_path.write_text(cpp_src)

        cmd = [
            "/usr/bin/c++",
            "-std=c++20",
            f"-I{cls.include_dir}",
            str(src_path),
            "-o",
            str(cls.bin_path),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if res.returncode != 0:
            raise RuntimeError(f"Compilation failed:\n{res.stderr}")

    @classmethod
    def tearDownClass(cls):
        cls.temp_dir.cleanup()

    def run_client(self, args):
        cmd = [str(self.bin_path)] + args
        res = subprocess.run(cmd, capture_output=True, text=True, check=True, timeout=5)
        lines = res.stdout.strip().splitlines()
        kv = {}
        for line in lines:
            if "=" in line:
                k, v = line.split("=", 1)
                kv[k] = v
        return kv

    def test_absent_flag_false(self):
        kv = self.run_client(["sample.elf"])
        self.assertEqual(kv.get("quiet"), "0")
        self.assertEqual(kv.get("elf_path"), "sample.elf")
        self.assertEqual(kv.get("enable_firmware"), "0")

    def test_quiet_flag_true_and_preserves_path(self):
        kv = self.run_client(["--quiet", "sample.elf"])
        self.assertEqual(kv.get("quiet"), "1")
        self.assertEqual(kv.get("elf_path"), "sample.elf")
        self.assertEqual(kv.get("enable_firmware"), "0")

    def test_firmware_flag_with_quiet_false(self):
        kv = self.run_client(["--firmware", "sample.elf"])
        self.assertEqual(kv.get("quiet"), "0")
        self.assertEqual(kv.get("enable_firmware"), "1")
        self.assertEqual(kv.get("elf_path"), "sample.elf")


if __name__ == "__main__":
    unittest.main()
