import unittest

from tests.fw_cpp_test_support import run_cpp


class TestFwUartOutput(unittest.TestCase):
    def test_service_retains_backlog_on_eagain_then_drains_in_order(self):
        source = r'''#include "tinyarmsim/utils/firmware_uart_output.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <fcntl.h>
#include <vector>
#include <unistd.h>
using tinyarmsim::FirmwareUartOutput;

int main() {
    int fds[2];
    assert(pipe(fds) == 0);
    assert(fcntl(fds[1], F_SETPIPE_SZ, 4096) >= 4096);
    std::array<uint8_t, 4096> full{};
    full.fill(0x5a);
    assert(write(fds[1], full.data(), full.size()) == static_cast<ssize_t>(full.size()));

    FirmwareUartOutput output(fds[1]);
    const std::array<uint8_t, 7> expected{0, 255, 1, 2, 0, 128, 255};
    for (uint8_t byte : expected) assert(output.enqueue(byte));
    assert(output.service() == 0);
    assert(output.pending_bytes() == expected.size());

    std::array<uint8_t, 8192> buffer{};
    assert(read(fds[0], buffer.data(), full.size()) == static_cast<ssize_t>(full.size()));
    while (output.pending_bytes() != 0) (void)output.service();
    std::array<uint8_t, 7> actual{};
    assert(read(fds[0], actual.data(), actual.size()) == static_cast<ssize_t>(actual.size()));
    assert(actual == expected);
    assert(output.pending_bytes() == 0);
    close(fds[0]); close(fds[1]);
    return 0;
}'''
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_service_reports_overflow_and_fatal_peer_error(self):
        source = r'''#include "tinyarmsim/utils/firmware_uart_output.hpp"
#include <cassert>
#include <cerrno>
#include <system_error>
#include <unistd.h>
using tinyarmsim::FirmwareUartOutput;

int main() {
    int fds[2]; assert(pipe(fds) == 0);
    {
        FirmwareUartOutput output(fds[1]);
        for (std::size_t i = 0; i < FirmwareUartOutput::kCapacity; ++i)
            assert(output.enqueue(static_cast<uint8_t>(i)));
        assert(!output.enqueue(1));
        try { (void)output.service(); assert(false); }
        catch (const std::system_error& error) {
            assert(error.code() == std::make_error_code(std::errc::no_buffer_space));
        }
        assert(output.pending_bytes() == FirmwareUartOutput::kCapacity);
    }
    close(fds[0]); close(fds[1]);

    assert(pipe(fds) == 0);
    FirmwareUartOutput output(fds[1]);
    close(fds[0]);
    assert(output.enqueue(0xa5));
    try { (void)output.service(); assert(false); }
    catch (const std::system_error& error) { assert(error.code().value() == EPIPE); }
    assert(output.pending_bytes() == 1);
    close(fds[1]);
    return 0;
}'''
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_flush_waits_for_reader_and_drains_exact_bytes(self):
        source = r'''#include "tinyarmsim/utils/firmware_uart_output.hpp"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
using tinyarmsim::FirmwareUartOutput;

int main() {
    int fds[2];
    assert(pipe(fds) == 0);
    assert(fcntl(fds[1], F_SETPIPE_SZ, 4096) >= 4096);
    FirmwareUartOutput output(fds[1]);
    std::vector<uint8_t> expected;
    expected.reserve(FirmwareUartOutput::kCapacity);
    for (std::size_t i = 0; i < FirmwareUartOutput::kCapacity; ++i) {
        const auto byte = static_cast<uint8_t>((i * 37u + 19u) & 0xffu);
        assert(output.enqueue(byte));
        expected.push_back(byte);
    }

    std::vector<uint8_t> actual;
    actual.reserve(expected.size());
    std::thread reader([&] {
        std::array<uint8_t, 2048> buffer{};
        while (actual.size() < expected.size()) {
            const ssize_t count = ::read(fds[0], buffer.data(), buffer.size());
            assert(count > 0);
            actual.insert(actual.end(), buffer.begin(), buffer.begin() + count);
        }
    });
    output.flush(std::chrono::seconds(2));
    reader.join();
    assert(actual == expected);
    assert(output.pending_bytes() == 0);
    close(fds[0]);
    close(fds[1]);
    return 0;
}'''
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_flush_timeout_is_bounded_and_retains_pending_bytes(self):
        source = r'''#include "tinyarmsim/utils/firmware_uart_output.hpp"
#include <cassert>
#include <chrono>
#include <system_error>
#include <fcntl.h>
#include <unistd.h>
using tinyarmsim::FirmwareUartOutput;

int main() {
    int fds[2];
    assert(pipe(fds) == 0);
    assert(fcntl(fds[1], F_SETPIPE_SZ, 4096) >= 4096);
    const int original_flags = fcntl(fds[1], F_GETFL);
    assert(original_flags >= 0);
    {
        FirmwareUartOutput output(fds[1]);
        for (std::size_t i = 0; i < FirmwareUartOutput::kCapacity; ++i)
            assert(output.enqueue(static_cast<uint8_t>(i)));
        bool timed_out = false;
        try { output.flush(std::chrono::milliseconds(100)); }
        catch (const std::system_error& error) {
            timed_out = true;
            assert(error.code() == std::make_error_code(std::errc::timed_out));
        }
        assert(timed_out);
        assert(output.pending_bytes() > 0);
        assert(output.pending_bytes() <= FirmwareUartOutput::kCapacity);
    }
    assert((fcntl(fds[1], F_GETFL) & O_NONBLOCK) == (original_flags & O_NONBLOCK));
    assert(fcntl(fds[1], F_GETFD) >= 0);
    close(fds[0]);
    close(fds[1]);
    return 0;
}'''
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_flush_reports_overflow_after_queue_drains(self):
        source = r'''#include "tinyarmsim/utils/firmware_uart_output.hpp"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <system_error>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
using tinyarmsim::FirmwareUartOutput;

int main() {
    int fds[2];
    assert(pipe(fds) == 0);
    assert(fcntl(fds[1], F_SETPIPE_SZ, 4096) >= 4096);
    FirmwareUartOutput output(fds[1]);
    for (std::size_t i = 0; i < FirmwareUartOutput::kCapacity; ++i)
        assert(output.enqueue(static_cast<uint8_t>(i)));
    assert(!output.enqueue(0xee));
    assert(output.failed());

    std::thread reader([&] {
        std::array<uint8_t, 4096> buffer{};
        std::size_t total = 0;
        while (total < FirmwareUartOutput::kCapacity) {
            const ssize_t count = ::read(fds[0], buffer.data(), buffer.size());
            assert(count > 0);
            total += static_cast<std::size_t>(count);
        }
    });
    bool overflow_reported = false;
    try { output.flush(std::chrono::seconds(2)); }
    catch (const std::system_error& error) {
        overflow_reported = true;
        assert(error.code() == std::make_error_code(std::errc::no_buffer_space));
    }
    reader.join();
    assert(overflow_reported);
    assert(output.pending_bytes() == 0);
    assert(output.failed());
    close(fds[0]);
    close(fds[1]);
    return 0;
}'''
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_flush_closed_peer_survives_default_sigpipe_and_latches_failure(self):
        source = r'''#include "tinyarmsim/utils/firmware_uart_output.hpp"
#include <cassert>
#include <chrono>
#include <csignal>
#include <system_error>
#include <sys/wait.h>
#include <unistd.h>
using tinyarmsim::FirmwareUartOutput;

int main() {
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        assert(sigaction(SIGPIPE, &action, nullptr) == 0);
        int fds[2];
        assert(pipe(fds) == 0);
        FirmwareUartOutput output(fds[1]);
        close(fds[0]);
        assert(output.enqueue('z'));
        try { output.flush(std::chrono::milliseconds(200)); }
        catch (const std::system_error& error) {
            if (error.code().value() == EPIPE && output.failed() &&
                output.pending_bytes() == 1) _exit(42);
            _exit(43);
        }
        _exit(44);
    }
    int status = 0;
    const pid_t waited = waitpid(child, &status, 0);
    assert(waited == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 42);
    return 0;
}'''
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_partial_pipe_writes_preserve_wrapped_byte_order(self):
        source = r'''#include "tinyarmsim/utils/firmware_uart_output.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <fcntl.h>
#include <vector>
#include <unistd.h>
using tinyarmsim::FirmwareUartOutput;

int main() {
    int fds[2];
    assert(pipe(fds) == 0);
    assert(fcntl(fds[1], F_SETPIPE_SZ, 4096) >= 4096);
    const int flags = fcntl(fds[1], F_GETFL);
    assert(flags >= 0 && fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) == 0);
    FirmwareUartOutput output(fds[1]);

    std::vector<uint8_t> expected;
    expected.reserve(FirmwareUartOutput::kCapacity + 32768);
    for (std::size_t i = 0; i < FirmwareUartOutput::kCapacity; ++i) {
        const auto byte = i == 0 ? uint8_t{0} :
                          i + 1 == FirmwareUartOutput::kCapacity ? uint8_t{255} :
                          static_cast<uint8_t>((i * 37u) & 0xffu);
        assert(output.enqueue(byte));
        expected.push_back(byte);
    }

    const std::size_t first_count = output.write_pending();
    assert(first_count > 0);
    assert(first_count < FirmwareUartOutput::kCapacity);
    assert(output.pending_bytes() == FirmwareUartOutput::kCapacity - first_count);

    for (std::size_t i = 0; i < first_count; ++i) {
        const auto byte = static_cast<uint8_t>((i * 53u + 11u) & 0xffu);
        assert(output.enqueue(byte));
        expected.push_back(byte);
    }

    std::vector<uint8_t> actual;
    actual.reserve(expected.size());
    std::array<uint8_t, 8192> buffer{};
    while (actual.size() < expected.size()) {
        ssize_t count;
        do { count = ::read(fds[0], buffer.data(), buffer.size()); }
        while (count == -1 && errno == EINTR);
        assert(count >= 0);
        if (count > 0) actual.insert(actual.end(), buffer.begin(), buffer.begin() + count);
        if (output.pending_bytes() > 0) (void)output.write_pending();
        assert(count > 0 || output.pending_bytes() > 0 || actual.size() == expected.size());
    }
    assert(actual == expected);
    assert(output.pending_bytes() == 0);
    close(fds[0]);
    close(fds[1]);
    return 0;
}'''
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_ring_and_descriptor_ownership(self):
        source = r'''#include "tinyarmsim/utils/firmware_uart_output.hpp"
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
using tinyarmsim::FirmwareUartOutput;

int main() {
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    const int original_flags = fcntl(pipe_fds[1], F_GETFL);
    assert(original_flags >= 0);
    {
        FirmwareUartOutput output(pipe_fds[1]);
        assert((fcntl(pipe_fds[1], F_GETFL) & O_NONBLOCK) != 0);
        for (std::size_t i = 0; i < FirmwareUartOutput::kCapacity; ++i)
            assert(output.enqueue(static_cast<uint8_t>(i)));
        assert(output.pending_bytes() == FirmwareUartOutput::kCapacity);
        assert(!output.enqueue(0xaa));
        assert(output.failed());
        assert(output.pending_bytes() == FirmwareUartOutput::kCapacity);
        pollfd readable{pipe_fds[0], POLLIN, 0};
        assert(poll(&readable, 1, 0) == 0);
    }
    assert((fcntl(pipe_fds[1], F_GETFL) & O_NONBLOCK) == (original_flags & O_NONBLOCK));
    assert(fcntl(pipe_fds[1], F_GETFD) >= 0);
    close(pipe_fds[0]);
    close(pipe_fds[1]);

    assert(pipe(pipe_fds) == 0);
    const int borrowed_fd = pipe_fds[1];
    const int observer_fd = dup(borrowed_fd);
    assert(observer_fd >= 0);
    int replacement[2];
    try {
        FirmwareUartOutput output(borrowed_fd);
        close(borrowed_fd);
        assert(pipe(replacement) == 0);
        assert(replacement[0] == borrowed_fd);
        const int replacement_flags = fcntl(replacement[0], F_GETFL);
        assert(replacement_flags >= 0);
        assert(fcntl(replacement[0], F_SETFL, replacement_flags | O_NONBLOCK) == 0);
        throw std::runtime_error("scope unwind");
    } catch (const std::runtime_error&) {}
    assert(fcntl(borrowed_fd, F_GETFL) >= 0);
    assert((fcntl(borrowed_fd, F_GETFL) & O_NONBLOCK) != 0);
    assert((fcntl(observer_fd, F_GETFL) & O_NONBLOCK) == 0);
    close(replacement[0]);
    close(replacement[1]);
    close(observer_fd);
    close(pipe_fds[0]);
    close(borrowed_fd);

    int bad_pipe[2];
    assert(pipe(bad_pipe) == 0);
    close(bad_pipe[1]);
    bool constructor_failed = false;
    try { FirmwareUartOutput output(bad_pipe[1]); }
    catch (...) { constructor_failed = true; }
    assert(constructor_failed);
    close(bad_pipe[0]);
    return 0;
}'''
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_closed_peer_write_preserves_thread_signal_state(self):
        source = r'''#include "tinyarmsim/utils/firmware_uart_output.hpp"
#include <cassert>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
using tinyarmsim::FirmwareUartOutput;

static void handler(int) {}

int main() {
    struct sigaction original_action{};
    struct sigaction action{};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGPIPE, &action, &original_action) == 0);

    sigset_t pipe_set, original_mask;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    assert(pthread_sigmask(SIG_BLOCK, &pipe_set, &original_mask) == 0);

    int fds[2];
    assert(pipe(fds) == 0);
    const int original_flags = fcntl(fds[1], F_GETFL);
    assert(original_flags >= 0);
    {
        FirmwareUartOutput output(fds[1]);
        close(fds[0]);

        // Establish a pending SIGPIPE that the write guard must leave intact.
        assert(raise(SIGPIPE) == 0);
        sigset_t pending;
        assert(sigpending(&pending) == 0);
        assert(sigismember(&pending, SIGPIPE) == 1);
        assert(output.enqueue('x'));
        for (int i = 0; i < 2; ++i) {
            bool caught = false;
            try { (void)output.write_pending(); }
            catch (const std::system_error& error) {
                caught = true;
                assert(error.code().value() == EPIPE);
            }
            assert(caught);
            assert(output.pending_bytes() == 1);
            assert(sigpending(&pending) == 0);
            assert(sigismember(&pending, SIGPIPE) == 1);
            sigset_t current_mask;
            assert(pthread_sigmask(SIG_SETMASK, nullptr, &current_mask) == 0);
            assert(sigismember(&current_mask, SIGPIPE) == 1);
        }
    }
    sigset_t pending;
    assert(sigpending(&pending) == 0);
    assert(sigismember(&pending, SIGPIPE) == 1);
    struct sigaction after_action{};
    assert(sigaction(SIGPIPE, nullptr, &after_action) == 0);
    assert(after_action.sa_handler == handler);
    assert(pthread_sigmask(SIG_SETMASK, &original_mask, nullptr) == 0);

    // Repeat with SIGPIPE originally unblocked and no preexisting pending signal.
    sigset_t unblocked_mask;
    assert(pthread_sigmask(SIG_SETMASK, &original_mask, &unblocked_mask) == 0);
    assert(sigismember(&unblocked_mask, SIGPIPE) == 0);
    assert(sigpending(&pending) == 0);
    assert(sigismember(&pending, SIGPIPE) == 0);
    int second_pipe[2];
    assert(pipe(second_pipe) == 0);
    {
        FirmwareUartOutput output(second_pipe[1]);
        close(second_pipe[0]);
        assert(output.enqueue('y'));
        bool caught = false;
        try { (void)output.write_pending(); }
        catch (const std::system_error& error) {
            caught = true;
            assert(error.code().value() == EPIPE);
        }
        assert(caught);
        sigset_t restored_mask;
        assert(pthread_sigmask(SIG_SETMASK, nullptr, &restored_mask) == 0);
        assert(sigismember(&restored_mask, SIGPIPE) == 0);
        assert(sigpending(&pending) == 0);
        assert(sigismember(&pending, SIGPIPE) == 0);
        assert(sigaction(SIGPIPE, nullptr, &after_action) == 0);
        assert(after_action.sa_handler == handler);
    }
    close(second_pipe[1]);
    assert(fcntl(fds[1], F_GETFD) >= 0);
    close(fds[1]);
    return 0;
}'''
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_default_sigpipe_disposition_returns_epipe_without_termination(self):
        source = r'''#include "tinyarmsim/utils/firmware_uart_output.hpp"
#include <cassert>
#include <cerrno>
#include <csignal>
#include <system_error>
#include <sys/wait.h>
#include <unistd.h>
using tinyarmsim::FirmwareUartOutput;

int main() {
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        assert(sigaction(SIGPIPE, &action, nullptr) == 0);
        int fds[2];
        assert(pipe(fds) == 0);
        FirmwareUartOutput output(fds[1]);
        close(fds[0]);
        assert(output.enqueue('z'));
        try { (void)output.write_pending(); }
        catch (const std::system_error& error) {
            if (error.code().value() == EPIPE) _exit(42);
            _exit(43);
        }
        _exit(44);
    }
    int status = 0;
    const pid_t waited = waitpid(child, &status, 0);
    assert(waited == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 42);
    return 0;
}'''
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_default_sigpipe_service_preserves_latched_flush_epipe(self):
        source = r'''#include "tinyarmsim/utils/firmware_uart_output.hpp"
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <system_error>
#include <unistd.h>
using tinyarmsim::FirmwareUartOutput;

int main() {
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGPIPE, &action, nullptr) == 0);
    int fds[2];
    assert(pipe(fds) == 0);
    FirmwareUartOutput output(fds[1]);
    close(fds[0]);
    assert(output.enqueue('z'));
    bool flush_caught_epipe = false;
    try { output.flush(std::chrono::milliseconds(200)); }
    catch (const std::system_error& error) {
        flush_caught_epipe = true;
        assert(error.code().value() == EPIPE);
    }
    assert(flush_caught_epipe);
    assert(output.failed());
    bool service_caught_epipe = false;
    try { (void)output.service(); }
    catch (const std::system_error& error) {
        service_caught_epipe = true;
        assert(error.code().value() == EPIPE);
    }
    assert(service_caught_epipe);
    assert(output.failed());
    close(fds[1]);
    return 0;
}'''
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)


if __name__ == "__main__":
    unittest.main()
