import subprocess
import tempfile
import unittest
from pathlib import Path


class TestFwQuietLifetime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = Path(__file__).resolve().parent.parent
        cls.temp_dir = tempfile.TemporaryDirectory()
        cls.bin_path = Path(cls.temp_dir.name) / "test_quiet_lifetime_runner"

        wrapper_cpp = """#include <iostream>
#include <string>
#include <vector>

#define main sim_cli_entry
#include "src/main.cpp"
#undef main

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <malformed-elf-path>\\n";
        return 2;
    }
    const char* bad_elf = argv[1];

    std::streambuf* orig_cout = std::cout.rdbuf();

    char arg0[] = "tinycpusim";
    char arg1[] = "--firmware";
    char arg2[] = "--quiet";
    char* arg3 = argv[1];
    char* sim_argv[] = {arg0, arg1, arg2, arg3, nullptr};

    int rc = sim_cli_entry(4, sim_argv);

    std::streambuf* post_cout = std::cout.rdbuf();

    // Failing test cleanup / guard: restore cout to avoid undefined teardown
    std::cout.rdbuf(orig_cout);

    if (post_cout != orig_cout) {
        std::cerr << "STREAMBUF_MISMATCH: cout.rdbuf() was not restored after exit!\\n";
        return 101;
    }

    if (rc != 1) {
        std::cerr << "UNEXPECTED_EXIT_CODE: " << rc << "\\n";
        return 102;
    }

    return 0;
}
"""
        src_path = Path(cls.temp_dir.name) / "test_runner.cpp"
        src_path.write_text(wrapper_cpp)

        cmd = [
            "/usr/bin/c++",
            "-std=c++20",
            f"-I{cls.repo_root / 'include'}",
            f"-I{cls.repo_root}",
            str(src_path),
            "-o",
            str(cls.bin_path),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"Wrapper compilation failed:\n{res.stderr}")

    @classmethod
    def tearDownClass(cls):
        cls.temp_dir.cleanup()

    def test_cout_restored_on_malformed_elf_under_firmware_quiet(self):
        # Existing malformed/unsupported ELF (x86_64 ELF)
        candidate_elfs = [
            self.repo_root / "build" / "CMakeFiles" / "3.28.3" / "CMakeDetermineCompilerABI_C.bin",
            self.repo_root / "build" / "CMakeFiles" / "3.28.3" / "CMakeDetermineCompilerABI_CXX.bin",
            self.repo_root / "CMakeLists.txt",
        ]
        bad_elf = None
        for cand in candidate_elfs:
            if cand.is_file():
                bad_elf = cand
                break
        self.assertIsNotNone(bad_elf, "No existing malformed/invalid ELF found")

        res = subprocess.run([str(self.bin_path), str(bad_elf)], capture_output=True, text=True)
        self.assertIn("Error loading ELF:", res.stderr)
        self.assertEqual(res.returncode, 0, f"Failed with rc={res.returncode}. Stderr: {res.stderr}")


if __name__ == "__main__":
    unittest.main()
