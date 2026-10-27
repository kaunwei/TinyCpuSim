import json
import pathlib
import unittest
from tests.fw_cpp_test_support import run_cpp

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

class TestFwControlRegsCli(unittest.TestCase):
    def test_cli_entry_firmware_control_regs_and_stop(self):
        main_cpp = str(_REPO_ROOT / "src" / "main.cpp")
        elf_path = str(_REPO_ROOT / "build" / "isa_boot_acceptance.elf")
        source = f"""#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <iostream>
#include <string>

#define main sim_cli_entry
#include "{main_cpp}"
#undef main

int main() {{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    // Preload commands before entry: regs\\nregs\\nstop\\n
    std::string cmds = "regs\\nregs\\nstop\\n";
    assert(write(sv[1], cmds.data(), cmds.size()) == static_cast<ssize_t>(cmds.size()));

    std::string fd_str = std::to_string(sv[0]);
    std::string elf = "{elf_path}";
    char prog[] = "tinyarmsim";
    char flag_fw[] = "--firmware";
    char flag_quiet[] = "--quiet";
    char flag_cfd[] = "--control-fd";
    char* argv[] = {{prog, flag_fw, flag_quiet, flag_cfd, fd_str.data(), elf.data(), nullptr}};
    int argc = 6;

    int ret = sim_cli_entry(argc, argv);
    assert(ret == 0);

    // After entry (exit 0), drain peer sv[1] and print to outer stdout
    char buf[1024];
    while (true) {{
        ssize_t n = read(sv[1], buf, sizeof(buf));
        if (n <= 0) break;
        std::cout.write(buf, n);
    }}
    close(sv[1]);
    return 0;
}}
"""
        proc = run_cpp(source, timeout=10)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}\nstdout:\n{proc.stdout}")

        # Parse drained stdout lines
        lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
        self.assertGreaterEqual(len(lines), 3, f"Expected at least 3 lines, got: {lines}")

        reg1 = json.loads(lines[0])
        reg2 = json.loads(lines[1])
        ack = json.loads(lines[2])

        # Assert two identical regs objects
        self.assertEqual(reg1, reg2)

        # Assert event, 16 unsigned regs, pc consistent with register 15, steps 0
        self.assertEqual(reg1["event"], "regs")
        self.assertIsInstance(reg1["registers"], list)
        self.assertEqual(len(reg1["registers"]), 16)
        for r in reg1["registers"]:
            self.assertIsInstance(r, int)
            self.assertGreaterEqual(r, 0)
        self.assertEqual(reg1["pc"], reg1["registers"][15])
        self.assertEqual(reg1["steps"], 0)

        # Assert stopped ack
        self.assertEqual(ack["event"], "stopped")
        self.assertEqual(ack["reason"], "requested")

if __name__ == "__main__":
    unittest.main()
