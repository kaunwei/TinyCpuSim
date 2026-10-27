import pathlib
import unittest
from tests.fw_cpp_test_support import run_cpp

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


class TestFwControlStepHalfcloseEntry(unittest.TestCase):

    def test_cli_entry_firmware_control_step_halfclose(self):
        main_cpp = str(_REPO_ROOT / "src" / "main.cpp")
        elf_path = str(_REPO_ROOT / "build" / "isa_boot_acceptance.elf")
        source = f"""#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <string>

#define main sim_cli_entry
#include "{main_cpp}"
#undef main

int main() {{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    std::string preload = "pause\\nstep\\nstop\\n";
    assert(write(sv[1], preload.data(), preload.size()) == static_cast<ssize_t>(preload.size()));
    assert(shutdown(sv[1], SHUT_WR) == 0);

    std::string fd_str = std::to_string(sv[0]);
    std::string elf = "{elf_path}";
    char prog[] = "tinyarmsim";
    char flag_fw[] = "--firmware";
    char flag_quiet[] = "--quiet";
    char flag_cfd[] = "--control-fd";
    char* argv[] = {{prog, flag_fw, flag_quiet, flag_cfd, fd_str.data(), elf.data(), nullptr}};
    int argc = 6;

    int ret = sim_cli_entry(argc, argv);
    close(sv[1]);
    return ret;
}}
"""
        proc = run_cpp(source, timeout=10)
        self.assertEqual(proc.returncode, 0, f"Expected returncode 0, got {proc.returncode}. stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")


if __name__ == "__main__":
    unittest.main()
