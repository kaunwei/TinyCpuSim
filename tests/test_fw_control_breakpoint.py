import unittest

from tests.fw_cpp_test_support import run_cpp


class TestFwControlBreakpoint(unittest.TestCase):
    def test_breakpoint_parsing_and_preserves_last_valid_address(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <cassert>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <unistd.h>

static std::string read_line(int fd) {
    pollfd pfd{fd, POLLIN, 0};
    assert(poll(&pfd, 1, 1000) == 1);
    std::string line;
    char c = 0;
    while (::read(fd, &c, 1) == 1 && c != '\n') line.push_back(c);
    return line;
}

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    tinyarmsim::FirmwareControl control(sv[0]);
    uint32_t pc = 0x10000;
    control.set_pc_provider([&]() { return pc; });
    tinyarmsim::FirmwareControl moved(std::move(control));

    const char commands[] = "break 0x10080\n";
    assert(write(sv[1], commands, sizeof(commands) - 1) == sizeof(commands) - 1);
    assert(!moved.stop_requested());
    assert(read_line(sv[1]) == "{\"event\":\"breakpoint_set\",\"pc\":65664}");

    const char invalid[] = "break -1\nbreak 4294967296\nbreak 0x10080junk\n";
    assert(write(sv[1], invalid, sizeof(invalid) - 1) == sizeof(invalid) - 1);
    assert(!moved.stop_requested());
    assert(read_line(sv[1]) == "{\"error\":\"invalid breakpoint address\"}");
    assert(read_line(sv[1]) == "{\"error\":\"invalid breakpoint address\"}");
    assert(read_line(sv[1]) == "{\"error\":\"invalid breakpoint address\"}");

    pc = 0x10080;
    std::thread runner([&]() { assert(moved.stop_requested()); });
    assert(read_line(sv[1]) == "{\"event\":\"breakpoint\",\"pc\":65664}");
    const char clear[] = "break clear\n";
    assert(write(sv[1], clear, sizeof(clear) - 1) == sizeof(clear) - 1);
    assert(read_line(sv[1]) == "{\"event\":\"breakpoint_cleared\"}");
    const char stop[] = "stop\n";
    assert(write(sv[1], stop, sizeof(stop) - 1) == sizeof(stop) - 1);
    assert(read_line(sv[1]) == "{\"event\":\"stopped\",\"reason\":\"requested\"}");
    runner.join();
    close(sv[1]);
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"stderr:\n{proc.stderr}")

    def test_continue_at_self_branch_rehits_at_next_boundary(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <cassert>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static std::string read_line(int fd) {
    pollfd pfd{fd, POLLIN, 0};
    assert(poll(&pfd, 1, 1000) == 1);
    std::string line;
    char c = 0;
    while (::read(fd, &c, 1) == 1 && c != '\n') line.push_back(c);
    return line;
}

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    tinyarmsim::FirmwareControl control(sv[0]);
    uint32_t pc = 0x20004;
    control.set_pc_provider([&]() { return pc; });
    assert(write(sv[1], "break 0x20000\n", 14) == 14);
    assert(!control.stop_requested());
    assert(read_line(sv[1]) == "{\"event\":\"breakpoint_set\",\"pc\":131072}");
    pc = 0x20000;

    std::thread runner([&]() {
        assert(!control.stop_requested());
        // Model the one self-branch instruction executed by run_until.
        pc = 0x20000;
        assert(control.stop_requested());
    });
    assert(read_line(sv[1]) == "{\"event\":\"breakpoint\",\"pc\":131072}");
    assert(write(sv[1], "continue\n", 9) == 9);
    assert(read_line(sv[1]) == "{\"event\":\"continued\"}");
    assert(read_line(sv[1]) == "{\"event\":\"breakpoint\",\"pc\":131072}");
    assert(write(sv[1], "stop\n", 5) == 5);
    assert(read_line(sv[1]) == "{\"event\":\"stopped\",\"reason\":\"requested\"}");
    runner.join();
    close(sv[1]);
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"stderr:\n{proc.stderr}")
