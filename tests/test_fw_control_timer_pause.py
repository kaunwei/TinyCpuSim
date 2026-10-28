import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwControlTimerPause(unittest.TestCase):

    def test_pause_freezes_timer_and_step_advances_one_tick(self):
        source = r"""
#include <tinyarmsim/firmware_control.hpp>
#include <tinyarmsim/interpreter.hpp>
#include <tinyarmsim/mmio/timer.hpp>
#include <tinyarmsim/memory_bus.hpp>
#include <tinyarmsim/state.hpp>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

static std::string read_line(int fd) {
    std::string line;
    while (true) {
        pollfd pfd{fd, POLLIN, 0};
        int ready;
        do {
            ready = ::poll(&pfd, 1, 1000);
        } while (ready < 0 && errno == EINTR);
        assert(ready == 1);
        char c = 0;
        ssize_t n;
        do {
            n = ::read(fd, &c, 1);
        } while (n < 0 && errno == EINTR);
        assert(n == 1);
        line.push_back(c);
        if (c == '\n') return line;
    }
}

static void send_command(int fd, const char* command) {
    const std::string line = std::string(command) + "\n";
    size_t sent = 0;
    while (sent < line.size()) {
        pollfd pfd{fd, POLLOUT, 0};
        int ready;
        do {
            ready = ::poll(&pfd, 1, 1000);
        } while (ready < 0 && errno == EINTR);
        assert(ready == 1);
        ssize_t n;
        do {
            n = ::write(fd, line.data() + sent, line.size() - sent);
        } while (n < 0 && errno == EINTR);
        assert(n > 0);
        sent += static_cast<size_t>(n);
    }
}

int main() {
    int sv[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    tinyarmsim::MemoryBus bus(64 * 1024 * 1024);
    tinyarmsim::ArchitecturalState state;
    tinyarmsim::IsaInterpreter interp(state, bus);
    auto timer_dev = std::make_unique<tinyarmsim::mmio::TimerDevice>();
    auto* timer = timer_dev.get();
    bus.register_device(std::move(timer_dev));

    // A known two-byte NOP at the interpreter entry point.
    bus.write16(0x0100, 0x46C0);
    state.set_pc(0x0100);
    state.set_reg(13, 0x10000);
    timer->write(0x00, 100, tinyarmsim::mmio::AccessSize::Word);
    timer->write(0x08, 0x01, tinyarmsim::mmio::AccessSize::Word); // enabled, IRQ disabled

    tinyarmsim::FirmwareControl control(sv[0]);
    control.set_register_provider([&]() {
        std::ostringstream out;
        out << "{\"event\":\"regs\",\"instructions\":" << interp.get_stats().instruction_count
            << ",\"timer\":" << bus.read32(0x40001004) << "}";
        return out.str();
    });

    send_command(sv[1], "pause");
    std::thread execution_thread([&]() {
        while (!control.stop_requested()) {
            interp.step();
        }
    });

    assert(read_line(sv[1]) == "{\"event\":\"paused\"}\n");

    send_command(sv[1], "regs");
    const std::string first = read_line(sv[1]);
    send_command(sv[1], "regs");
    const std::string second = read_line(sv[1]);
    assert(first == second);
    assert(first == "{\"event\":\"regs\",\"instructions\":0,\"timer\":100}\n");

    send_command(sv[1], "step");
    assert(read_line(sv[1]) == "{\"event\":\"stepped\"}\n");

    send_command(sv[1], "regs");
    assert(read_line(sv[1]) == "{\"event\":\"regs\",\"instructions\":1,\"timer\":99}\n");

    send_command(sv[1], "stop");
    assert(read_line(sv[1]) == "{\"event\":\"stopped\",\"reason\":\"requested\"}\n");
    execution_thread.join();
    assert(control.stop_requested());
    assert(!control.closed_without_stop());
    ::close(sv[1]);
    return 0;
}
"""
        proc = run_cpp(source, timeout=10)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}\nstdout:\n{proc.stdout}")


if __name__ == "__main__":
    unittest.main()
