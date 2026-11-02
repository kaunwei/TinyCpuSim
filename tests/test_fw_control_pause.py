import json
import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwControlPause(unittest.TestCase):

    def test_pause_regs_continue_and_stop_cycle(self):
        source = r"""
#include <tinyarmsim/utils/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>

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

    std::atomic<uint64_t> step_counter{0};
    std::atomic<bool> thread_done{false};

    control.set_register_provider([&]() {
        std::ostringstream oss;
        oss << "{\"event\":\"regs\",\"r0\":" << step_counter.load() << "}";
        return oss.str();
    });

    std::thread host_thread([&]() {
        while (!control.stop_requested()) {
            step_counter.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        thread_done.store(true);
    });

    // Let the worker run a few steps first
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    assert(step_counter.load() > 0);

    // Send pause command
    assert(::write(sv[1], "pause\n", 6) == 6);
    std::string resp = read_line(sv[1]);
    assert(resp == "{\"event\":\"paused\"}\n");

    // Once paused event is received, host_thread is blocked inside stop_requested poll
    uint64_t snapshot1_step = step_counter.load();

    // Query regs multiple times
    assert(::write(sv[1], "regs\n", 5) == 5);
    std::string regs1 = read_line(sv[1]);

    // Give some time to confirm step counter is strictly frozen while paused
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    uint64_t snapshot2_step = step_counter.load();
    assert(snapshot1_step == snapshot2_step);

    assert(::write(sv[1], "regs\n", 5) == 5);
    std::string regs2 = read_line(sv[1]);
    assert(regs1 == regs2);

    // Continue execution
    assert(::write(sv[1], "continue\n", 9) == 9);
    resp = read_line(sv[1]);
    assert(resp == "{\"event\":\"continued\"}\n");

    // Verify progress resumes
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    assert(step_counter.load() > snapshot2_step);

    // Stop execution
    assert(::write(sv[1], "stop\n", 5) == 5);
    resp = read_line(sv[1]);
    assert(resp == "{\"event\":\"stopped\",\"reason\":\"requested\"}\n");

    host_thread.join();
    assert(thread_done.load());
    assert(control.stop_requested());
    assert(!control.closed_without_stop());

    ::close(sv[1]);
    return 0;
}
"""
        proc = run_cpp(source, timeout=10)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_regs_missing_provider(self):
        source = r"""
#include <tinyarmsim/utils/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <string>

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    tinyarmsim::FirmwareControl control(sv[0]);

    assert(::write(sv[1], "regs\n", 5) == 5);
    assert(!control.stop_requested());

    char buf[128] = {0};
    ssize_t n = ::read(sv[1], buf, sizeof(buf) - 1);
    assert(n > 0);
    std::string resp(buf, n);
    assert(resp == "{\"error\":\"missing register provider\"}\n");

    assert(::write(sv[1], "stop\n", 5) == 5);
    assert(control.stop_requested());

    ::close(sv[1]);
    return 0;
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_pause_then_eof(self):
        source = r"""
#include <tinyarmsim/utils/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <string>
#include <thread>
#include <chrono>

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    tinyarmsim::FirmwareControl control(sv[0]);

    std::thread host_thread([&]() {
        while (!control.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    assert(::write(sv[1], "pause\n", 6) == 6);
    char buf[128] = {0};
    ssize_t n = ::read(sv[1], buf, sizeof(buf) - 1);
    assert(n > 0);
    std::string resp(buf, n);
    assert(resp == "{\"event\":\"paused\"}\n");

    // Close while paused
    ::close(sv[1]);

    host_thread.join();
    assert(control.stop_requested());
    assert(control.closed_without_stop());

    return 0;
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_pause_then_stop(self):
        source = r"""
#include <tinyarmsim/utils/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <string>
#include <thread>
#include <chrono>

int main() {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    tinyarmsim::FirmwareControl control(sv[0]);

    std::thread host_thread([&]() {
        while (!control.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    assert(::write(sv[1], "pause\n", 6) == 6);
    char buf[128] = {0};
    ssize_t n = ::read(sv[1], buf, sizeof(buf) - 1);
    assert(n > 0);
    std::string resp(buf, n);
    assert(resp == "{\"event\":\"paused\"}\n");

    assert(::write(sv[1], "stop\n", 5) == 5);
    n = ::read(sv[1], buf, sizeof(buf) - 1);
    assert(n > 0);
    resp = std::string(buf, n);
    assert(resp == "{\"event\":\"stopped\",\"reason\":\"requested\"}\n");

    host_thread.join();
    assert(control.stop_requested());
    assert(!control.closed_without_stop());

    ::close(sv[1]);
    return 0;
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")


if __name__ == "__main__":
    unittest.main()
