import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwControlStepRunning(unittest.TestCase):

    def test_step_while_running_returns_error_and_preserves_state(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <string>
#include <vector>

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

    // Preload: step\nstop\n
    std::string preload = "step\nstop\n";
    assert(::write(sv[1], preload.data(), preload.size()) == static_cast<ssize_t>(preload.size()));

    // First call to stop_requested() processes preload lines while not paused.
    // 'step' while running must emit error "step requires pause" and NOT change execution permission or stop state.
    // 'stop' immediately follows and sets stop_requested_ = true.
    bool stopped = control.stop_requested();
    assert(stopped == true);
    assert(control.stop_requested() == true);
    assert(!control.closed_without_stop());
    assert(!control.transport_failed());

    // Read responses: first error, then requested stopped acknowledgement
    std::string line1 = read_line(sv[1]);
    std::string line2 = read_line(sv[1]);

    assert(line1 == "{\"error\":\"step requires pause\"}\n");
    assert(line2 == "{\"event\":\"stopped\",\"reason\":\"requested\"}\n");

    ::close(sv[1]);
    return 0;
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}\nstdout:\n{proc.stdout}")


if __name__ == "__main__":
    unittest.main()
