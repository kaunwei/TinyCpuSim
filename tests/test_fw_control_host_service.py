import unittest

from tests.fw_cpp_test_support import run_cpp


class TestFwControlHostService(unittest.TestCase):
    def test_host_service_runs_for_running_checks_and_step_grants(self):
        source = r"""
#include <tinyarmsim/utils/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <string>

static std::string read_line(int fd) {
    std::string line;
    char c = 0;
    while (::read(fd, &c, 1) == 1) {
        line.push_back(c);
        if (c == '\n') return line;
    }
    return line;
}

static void running_check_calls_host_service() {
    int sv[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    tinyarmsim::FirmwareControl control(sv[0]);
    int host_calls = 0;
    control.set_host_service([&] { ++host_calls; });
    assert(!control.stop_requested());
    assert(host_calls == 1);
    assert(!control.stop_requested());
    assert(host_calls == 2);
    ::close(sv[1]);
}

static void initial_step_grant_calls_host_service() {
    int sv[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    tinyarmsim::FirmwareControl control(sv[0]);
    uint64_t instructions = 0;
    int host_calls = 0;
    control.set_instruction_counter([&] { return instructions; });
    control.set_host_service([&] { ++host_calls; });
    assert(::write(sv[1], "pause\nstep\n", 11) == 11);
    assert(!control.stop_requested());
    assert(host_calls == 1);
    assert(instructions == 0);
    assert(read_line(sv[1]) == "{\"event\":\"paused\"}\n");
    ::close(sv[1]);
}

static void unchanged_step_progress_calls_host_service() {
    int sv[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    tinyarmsim::FirmwareControl control(sv[0]);
    uint64_t instructions = 0;
    int host_calls = 0;
    control.set_instruction_counter([&] { return instructions; });
    control.set_host_service([&] { ++host_calls; });
    assert(::write(sv[1], "pause\nstep\n", 11) == 11);
    assert(!control.stop_requested());
    assert(host_calls == 1);
    assert(!control.stop_requested());
    assert(host_calls == 2);
    assert(instructions == 0);
    ::close(sv[1]);
}

int main() {
    running_check_calls_host_service();
    initial_step_grant_calls_host_service();
    unchanged_step_progress_calls_host_service();
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_host_service_runs_while_paused_until_peer_stops(self):
        source = r"""
#include <tinyarmsim/utils/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

static std::string read_line(int fd) {
    std::string line;
    char c = 0;
    while (::read(fd, &c, 1) == 1) {
        line.push_back(c);
        if (c == '\n') return line;
    }
    return line;
}

int main() {
    int sv[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    tinyarmsim::FirmwareControl control(sv[0]);
    int guest_instructions = 0;
    int timer_ticks = 0;
    int host_calls = 0;
    control.set_instruction_counter([&] { return static_cast<uint64_t>(guest_instructions); });
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    control.set_host_service([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++host_calls;
        ready = host_calls >= 3;
        changed.notify_one();
    });
    assert(::write(sv[1], "pause\n", 6) == 6);
    std::thread dispatcher([&] { assert(control.stop_requested()); });
    assert(read_line(sv[1]) == "{\"event\":\"paused\"}\n");
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return ready; });
    }
    assert(guest_instructions == 0);
    assert(timer_ticks == 0);
    assert(::write(sv[1], "stop\n", 5) == 5);
    assert(read_line(sv[1]) == "{\"event\":\"stopped\",\"reason\":\"requested\"}\n");
    dispatcher.join();
    assert(host_calls >= 3);
    assert(guest_instructions == 0 && timer_ticks == 0);
    ::close(sv[1]);
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_host_service_moves_with_control(self):
        source = r"""
#include <tinyarmsim/utils/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <atomic>
#include <string>
#include <thread>

static std::string read_line(int fd) {
    std::string line;
    char c = 0;
    while (::read(fd, &c, 1) == 1) {
        line.push_back(c);
        if (c == '\n') return line;
    }
    return line;
}

int main() {
    int sv[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    std::atomic<int> calls{0};
    tinyarmsim::FirmwareControl original(sv[0]);
    original.set_host_service([&] { ++calls; });
    tinyarmsim::FirmwareControl moved(std::move(original));
    assert(::write(sv[1], "pause\n", 6) == 6);
    std::thread dispatcher([&] { assert(moved.stop_requested()); });
    assert(read_line(sv[1]) == "{\"event\":\"paused\"}\n");
    std::thread stopper([&] {
        while (calls.load() == 0) std::this_thread::yield();
        assert(::write(sv[1], "stop\n", 5) == 5);
    });
    stopper.join();
    dispatcher.join();
    assert(calls > 0);
    ::close(sv[1]);
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_host_service_exception_propagates_and_closes_control_fd(self):
        source = r"""
#include <tinyarmsim/utils/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <stdexcept>

int main() {
    int sv[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int controlled_fd = sv[0];
    bool caught = false;
    try {
        tinyarmsim::FirmwareControl control(controlled_fd);
        control.set_host_service([] { throw std::runtime_error("host service failed"); });
        assert(::write(sv[1], "pause\n", 6) == 6);
        control.stop_requested();
    } catch (const std::runtime_error& error) {
        caught = std::string(error.what()) == "host service failed";
    }
    assert(caught);
    assert(::fcntl(controlled_fd, F_GETFD) == -1);
    ::close(sv[1]);
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")

    def test_empty_host_service_preserves_stop_and_eof_behavior(self):
        source = r"""
#include <tinyarmsim/utils/firmware_control.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>

int main() {
    int sv[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    tinyarmsim::FirmwareControl control(sv[0]);
    assert(::write(sv[1], "stop\n", 5) == 5);
    assert(control.stop_requested());
    ::close(sv[1]);

    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    tinyarmsim::FirmwareControl eof_control(sv[0]);
    ::close(sv[1]);
    assert(eof_control.stop_requested());
    assert(eof_control.closed_without_stop());
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}")


if __name__ == "__main__":
    unittest.main()
