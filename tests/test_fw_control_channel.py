import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwControlChannel(unittest.TestCase):

    def test_partial_stop_then_p_and_ack(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <string>

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    tinyarmsim::FirmwareControl control(sv[0]);

    // Partial input: "sto" without newline
    assert(write(sv[1], "sto", 3) == 3);
    assert(!control.stop_requested());
    assert(!control.closed_without_stop());

    // Complete the line: "p\n"
    assert(write(sv[1], "p\n", 2) == 2);
    assert(control.stop_requested());
    assert(!control.closed_without_stop());

    // Read ack from sv[1]
    char buf[128] = {0};
    ssize_t n = read(sv[1], buf, sizeof(buf) - 1);
    assert(n > 0);
    std::string ack(buf, n);
    assert(ack == "{\"event\":\"stopped\",\"reason\":\"requested\"}\n");

    close(sv[1]);
    return 0;
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_one_shot_result_no_duplicate_ack(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cassert>
#include <string>

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    tinyarmsim::FirmwareControl control(sv[0]);

    // Send "stop\n"
    assert(write(sv[1], "stop\n", 5) == 5);
    assert(control.stop_requested());

    // Read the single ack
    char buf[128] = {0};
    ssize_t n = read(sv[1], buf, sizeof(buf) - 1);
    assert(n > 0);
    std::string ack(buf, n);
    assert(ack == "{\"event\":\"stopped\",\"reason\":\"requested\"}\n");

    // Make sv[1] nonblocking to verify no duplicate ack is sent on repeated calls
    int flags = fcntl(sv[1], F_GETFL, 0);
    fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);

    // Repeated calls
    assert(control.stop_requested());
    assert(control.stop_requested());

    // No additional data should be sent
    char buf2[128];
    ssize_t n2 = read(sv[1], buf2, sizeof(buf2));
    assert(n2 == -1);

    close(sv[1]);
    return 0;
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_eof_before_stop(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    tinyarmsim::FirmwareControl control(sv[0]);

    // Close sv[1] before sending stop (EOF)
    close(sv[1]);

    // Stop requested should become true and closed_without_stop should be marked
    assert(control.stop_requested());
    assert(control.closed_without_stop());

    // Repeated call remains true
    assert(control.stop_requested());
    assert(control.closed_without_stop());

    return 0;
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_unknown_command_followed_by_stop(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <string>

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    tinyarmsim::FirmwareControl control(sv[0]);

    // Send unknown command
    assert(write(sv[1], "status\n", 7) == 7);
    assert(!control.stop_requested());
    assert(!control.closed_without_stop());

    // Read error response
    char buf[128] = {0};
    ssize_t n = read(sv[1], buf, sizeof(buf) - 1);
    assert(n > 0);
    std::string err(buf, n);
    assert(err == "{\"error\":\"unknown command\"}\n");

    // Now send stop
    assert(write(sv[1], "stop\n", 5) == 5);
    assert(control.stop_requested());
    assert(!control.closed_without_stop());

    // Read stop ack
    n = read(sv[1], buf, sizeof(buf) - 1);
    assert(n > 0);
    std::string ack(buf, n);
    assert(ack == "{\"event\":\"stopped\",\"reason\":\"requested\"}\n");

    close(sv[1]);
    return 0;
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")


if __name__ == "__main__":
    unittest.main()
