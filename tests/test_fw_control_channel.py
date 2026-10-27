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


    def test_stop_followed_by_half_close(self):
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

    // Send "stop\n" and immediately half-close write side
    assert(write(sv[1], "stop\n", 5) == 5);
    assert(shutdown(sv[1], SHUT_WR) == 0);

    // Now call stop_requested
    assert(control.stop_requested());
    assert(!control.closed_without_stop());

    // Exact stopped ack should be received on sv[1]
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

    def test_partial_incomplete_stop_plus_eof(self):
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

    // Send incomplete stop "sto" and close
    assert(write(sv[1], "sto", 3) == 3);
    close(sv[1]);

    // Should result in stop_requested true and closed_without_stop true
    assert(control.stop_requested());
    assert(control.closed_without_stop());

    return 0;
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_stop_followed_by_full_close_no_sigpipe(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    tinyarmsim::FirmwareControl control(sv[0]);

    // Peer writes "stop\n" and immediately closes entirely (not half-close)
    assert(write(sv[1], "stop\n", 5) == 5);
    close(sv[1]);

    // Control owner processes the command; sending ack must not trigger SIGPIPE
    assert(control.stop_requested());
    assert(!control.closed_without_stop());

    return 0;
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_backpressured_peer_drains_and_receives_all(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    // Limit send buffer on control socket to 1024 bytes (or min allowed by kernel)
    int sndbuf = 1024;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    int rcvbuf = 1024;
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    tinyarmsim::FirmwareControl control(sv[0]);
    control.set_register_provider([]() {
        return "{\"r0\":0,\"r1\":1,\"r2\":2,\"r3\":3,\"r4\":4,\"r5\":5,\"r6\":6,\"r7\":7}";
    });

    // Queue 100 regs commands followed by stop\n
    std::string cmd;
    for (int i = 0; i < 100; ++i) {
        cmd += "regs\n";
    }
    cmd += "stop\n";

    // Write all commands from sv[1]
    size_t written = 0;
    while (written < cmd.size()) {
        ssize_t n = write(sv[1], cmd.data() + written, cmd.size() - written);
        assert(n > 0);
        written += n;
    }

    // Launch background thread that delays 20ms then drains sv[1] until stopped ack is received
    std::string received;
    std::thread reader([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        char buf[512];
        while (true) {
            ssize_t n = read(sv[1], buf, sizeof(buf));
            if (n > 0) {
                received.append(buf, n);
                if (received.find("{\"event\":\"stopped\",\"reason\":\"requested\"}\n") != std::string::npos) {
                    break;
                }
            } else if (n == 0) {
                break;
            } else {
                if (errno == EINTR) continue;
                break;
            }
        }
    });

    // Control processes commands
    bool stopped = control.stop_requested();
    assert(stopped);
    assert(!control.transport_failed());
    assert(!control.closed_without_stop());

    reader.join();
    close(sv[1]);

    // Verify all responses received: 100 register responses + 1 stopped ack
    size_t count = 0;
    size_t pos = 0;
    while ((pos = received.find('\n', pos)) != std::string::npos) {
        count++;
        pos++;
    }
    assert(count == 101);
    assert(received.find("{\"event\":\"stopped\",\"reason\":\"requested\"}\n") != std::string::npos);

    return 0;
}
"""
        proc = run_cpp(source, timeout=10)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_permanently_non_reading_peer_yields_transport_failed(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <string>

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    int sndbuf = 1024;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    int rcvbuf = 1024;
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    tinyarmsim::FirmwareControl control(sv[0]);
    control.set_register_provider([]() {
        return "{\"r0\":0,\"r1\":1,\"r2\":2,\"r3\":3,\"r4\":4,\"r5\":5,\"r6\":6,\"r7\":7}";
    });

    // Queue 100 regs commands followed by stop\n
    std::string cmd;
    for (int i = 0; i < 100; ++i) {
        cmd += "regs\n";
    }
    cmd += "stop\n";

    size_t written = 0;
    while (written < cmd.size()) {
        ssize_t n = write(sv[1], cmd.data() + written, cmd.size() - written);
        assert(n > 0);
        written += n;
    }

    // sv[1] never reads anything.
    auto t0 = std::chrono::steady_clock::now();
    bool stopped = control.stop_requested();
    auto t1 = std::chrono::steady_clock::now();

    // Must return within bounded time (deadline ~250ms per stuck send, overall bounded)
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    assert(stopped);
    assert(control.transport_failed());
    // Do NOT mislabel as EOF before stop!
    assert(!control.closed_without_stop());
    assert(elapsed_ms >= 200 && elapsed_ms < 1000);

    // Test move preservation
    tinyarmsim::FirmwareControl moved(std::move(control));
    assert(moved.transport_failed());
    assert(!moved.closed_without_stop());
    assert(moved.stop_requested());

    close(sv[1]);
    return 0;
}
"""
        proc = run_cpp(source, timeout=10)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")



if __name__ == "__main__":
    unittest.main()
