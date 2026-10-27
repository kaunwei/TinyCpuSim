import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwControlStepDeferStop(unittest.TestCase):

    def test_step_defer_stop(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <string>

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    std::string preload = "pause\nstep\nstop\n";
    assert(::write(sv[1], preload.data(), preload.size()) == static_cast<ssize_t>(preload.size()));
    assert(::shutdown(sv[1], SHUT_WR) == 0);

    tinyarmsim::FirmwareControl control(sv[0]);

    int count = 0;
    while (!control.stop_requested()) {
        count++;
    }

    assert(count == 1);
    assert(!control.closed_without_stop());

    ::close(sv[1]);
    return 0;
}
"""
        proc = run_cpp(source, timeout=3)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}\nstdout:\n{proc.stdout}")


if __name__ == "__main__":
    unittest.main()
