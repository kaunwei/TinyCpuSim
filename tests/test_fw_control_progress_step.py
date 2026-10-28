import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwControlProgressStep(unittest.TestCase):

    def test_progress_acknowledgement_and_terminal_ordering(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

static std::string read_line(int fd) {
    std::string line;
    char c = 0;
    while (true) {
        ssize_t n = ::read(fd, &c, 1);
        assert(n == 1);
        line.push_back(c);
        if (c == '\n') return line;
    }
}

static std::vector<std::string> read_lines(int fd, size_t count) {
    std::vector<std::string> lines;
    for (size_t i = 0; i < count; ++i) lines.push_back(read_line(fd));
    return lines;
}

static void progress_case(bool half_close) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    uint64_t instructions = 0;
    tinyarmsim::FirmwareControl control(sv[0]);
    control.set_instruction_counter([&]() { return instructions; });
    control.set_register_provider([&]() {
        return "{\"event\":\"regs\",\"steps\":" + std::to_string(instructions) + "}";
    });
    const std::string preload = "pause\nstep\nregs\ncontinue\nstop\n";
    assert(::write(sv[1], preload.data(), preload.size()) == static_cast<ssize_t>(preload.size()));
    assert(control.stop_requested() == false);
    assert(read_line(sv[1]) == "{\"event\":\"paused\"}\n");

    tinyarmsim::FirmwareControl moved(std::move(control));
    assert(moved.stop_requested() == false);
    instructions = 1;
    assert(moved.stop_requested() == true);
    if (half_close) assert(::shutdown(sv[1], SHUT_WR) == 0);
    const auto lines = read_lines(sv[1], 4);
    assert(lines[0] == "{\"event\":\"stepped\"}\n");
    assert(lines[1] == "{\"event\":\"regs\",\"steps\":1}\n");
    assert(lines[2] == "{\"event\":\"continued\"}\n");
    assert(lines[3] == "{\"event\":\"stopped\",\"reason\":\"requested\"}\n");
    assert(!moved.closed_without_stop());
    ::close(sv[1]);
}

static void no_provider_fallback() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    tinyarmsim::FirmwareControl control(sv[0]);
    const std::string preload = "pause\nstep\nstop\n";
    assert(::write(sv[1], preload.data(), preload.size()) == static_cast<ssize_t>(preload.size()));
    assert(control.stop_requested() == false);
    assert(read_line(sv[1]) == "{\"event\":\"paused\"}\n");
    assert(control.stop_requested() == true);
    const auto lines = read_lines(sv[1], 2);
    assert(lines[0] == "{\"event\":\"stepped\"}\n");
    assert(lines[1] == "{\"event\":\"stopped\",\"reason\":\"requested\"}\n");
    ::close(sv[1]);
}

static void eof_precedes_step_acknowledgement() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    uint64_t instructions = 0;
    tinyarmsim::FirmwareControl control(sv[0]);
    control.set_instruction_counter([&]() { return instructions; });
    const std::string preload = "pause\nstep\n";
    assert(::write(sv[1], preload.data(), preload.size()) == static_cast<ssize_t>(preload.size()));
    assert(control.stop_requested() == false);
    assert(read_line(sv[1]) == "{\"event\":\"paused\"}\n");
    assert(::shutdown(sv[1], SHUT_WR) == 0);
    assert(control.stop_requested() == true);
    assert(control.closed_without_stop());
    ::close(sv[1]);
}

int main() {
    progress_case(false);
    progress_case(true);
    no_provider_fallback();
    eof_precedes_step_acknowledgement();
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}\nstdout:\n{proc.stdout}")


if __name__ == "__main__":
    unittest.main()
