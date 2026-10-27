import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwControlStepGrant(unittest.TestCase):

    def test_single_step_grant_ordered(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <string>
#include <vector>
#include <iostream>

static std::string read_line(int fd) {
    std::string line;
    char c = 0;
    while (true) {
        ssize_t n = ::read(fd, &c, 1);
        if (n <= 0) {
            break;
        }
        line.push_back(c);
        if (c == '\n') {
            break;
        }
    }
    return line;
}

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    tinyarmsim::FirmwareControl control(sv[0]);

    uint64_t step_counter = 0;

    control.set_register_provider([&]() {
        return "{\"event\":\"regs\",\"steps\":" + std::to_string(step_counter) + "}";
    });

    // Deterministic preload: pause, step, regs, stop with newlines
    std::string preload = "pause\nstep\nregs\nstop\n";
    assert(::write(sv[1], preload.data(), preload.size()) == static_cast<ssize_t>(preload.size()));

    // Controlled run simulation loop
    while (!control.stop_requested()) {
        step_counter++;
    }

    // Step counter must be exactly 1
    assert(step_counter == 1);

    // Read responses from sv[1]
    std::vector<std::string> lines;
    for (int i = 0; i < 4; ++i) {
        std::string line = read_line(sv[1]);
        lines.push_back(line);
    }

    assert(lines.size() == 4);
    assert(lines[0] == "{\"event\":\"paused\"}\n");
    assert(lines[1] == "{\"event\":\"stepped\"}\n");
    assert(lines[2] == "{\"event\":\"regs\",\"steps\":1}\n");
    assert(lines[3] == "{\"event\":\"stopped\",\"reason\":\"requested\"}\n");

    ::close(sv[1]);
    return 0;
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}\nstdout:\n{proc.stdout}")


if __name__ == "__main__":
    unittest.main()
