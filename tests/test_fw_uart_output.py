import unittest

from tests.fw_cpp_test_support import run_cpp


class TestFwUartOutput(unittest.TestCase):
    def test_ring_and_descriptor_ownership(self):
        source = r'''#include "tinyarmsim/firmware_uart_output.hpp"
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
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
    const int saved_flags = fcntl(pipe_fds[1], F_GETFL);
    const int borrowed_fd = pipe_fds[1];
    int replacement[2];
    try {
        FirmwareUartOutput output(borrowed_fd);
        close(borrowed_fd);
        assert(pipe(replacement) == 0);
        assert(replacement[0] == borrowed_fd);
        throw std::runtime_error("scope unwind");
    } catch (const std::runtime_error&) {}
    assert(fcntl(borrowed_fd, F_GETFL) >= 0);
    assert((fcntl(borrowed_fd, F_GETFL) & O_NONBLOCK) == 0);
    close(replacement[0]);
    close(replacement[1]);
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


if __name__ == "__main__":
    unittest.main()
