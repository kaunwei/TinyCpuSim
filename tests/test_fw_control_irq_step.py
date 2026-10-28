import unittest

from tests.fw_cpp_test_support import run_cpp


class TestFwControlIrqStep(unittest.TestCase):

    def test_one_step_executes_pending_irq_handler_instruction(self):
        source = r"""
#include "tinyarmsim/firmware_control.hpp"
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/mmio/nvic.hpp"
#include "tinyarmsim/mmio/timer.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Snapshot { uint64_t steps; uint32_t pc, r4, timer; };

static std::string read_line(int fd) {
    std::string line;
    char c = 0;
    while (true) {
        assert(::read(fd, &c, 1) == 1);
        line.push_back(c);
        if (c == '\n') return line;
    }
}

int main() {
    tinyarmsim::MemoryBus bus(64 * 1024 * 1024);
    tinyarmsim::ArchitecturalState state;
    tinyarmsim::IsaInterpreter interpreter(state, bus);
    auto timer_owner = std::make_unique<tinyarmsim::mmio::TimerDevice>();
    auto nvic_owner = std::make_unique<tinyarmsim::mmio::NVICDevice>();
    auto* timer = timer_owner.get();
    auto* nvic = nvic_owner.get();
    bus.register_device(std::move(timer_owner));
    bus.register_device(std::move(nvic_owner));
    interpreter.attach_nvic(nvic);
    nvic->set_vtor(0x1000);
    bus.write32(0x1044, 0x2001);
    nvic->write(0x100, 1u << 1, tinyarmsim::mmio::AccessSize::Word);
    bus.write16(0x2000, 0x2463); // MOVS R4,#99
    bus.write16(0x2002, 0x4770); // BX LR
    bus.write16(0x100, 0x46c0);  // Main NOP
    state.set_reg(13, 0x10000);
    state.set_pc(0x100);
    state.set_reg(4, 0);
    timer->write(0, 100, tinyarmsim::mmio::AccessSize::Word);
    timer->write(8, 7, tinyarmsim::mmio::AccessSize::Word);
    timer->tick(100);
    assert(timer->has_pending_irq());

    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    tinyarmsim::FirmwareControl control(sockets[0]);
    control.set_instruction_counter([&]() {
        return interpreter.get_stats().instruction_count;
    });
    std::vector<Snapshot> observed;
    control.set_register_provider([&]() {
        observed.push_back({interpreter.get_stats().instruction_count, state.get_pc(),
                            state.get_reg(4), timer->read(4, tinyarmsim::mmio::AccessSize::Word)});
        return std::string("{\"event\":\"regs\"}");
    });
    const std::string commands = "pause\nregs\nstep\nregs\nstop\n";
    assert(::write(sockets[1], commands.data(), commands.size()) == static_cast<ssize_t>(commands.size()));
    const auto result = interpreter.run_until([&]() { return control.stop_requested(); }, 100);
    assert(result == 0);
    assert(observed.size() == 2);
    assert(observed[0].steps == 0 && observed[0].pc == 0x100 &&
           observed[0].r4 == 0 && observed[0].timer == 100);
    assert(observed[1].steps == 1 && observed[1].pc == 0x2002 &&
           observed[1].r4 == 99 && observed[1].timer == 98);
    assert(read_line(sockets[1]) == "{\"event\":\"paused\"}\n");
    assert(read_line(sockets[1]) == "{\"event\":\"regs\"}\n");
    assert(read_line(sockets[1]) == "{\"event\":\"stepped\"}\n");
    assert(read_line(sockets[1]) == "{\"event\":\"regs\"}\n");
    assert(read_line(sockets[1]) == "{\"event\":\"stopped\",\"reason\":\"requested\"}\n");
    ::close(sockets[1]);
}
"""
        proc = run_cpp(source, timeout=5)
        self.assertEqual(proc.returncode, 0, f"Failed with stderr:\n{proc.stderr}\nstdout:\n{proc.stdout}")


if __name__ == "__main__":
    unittest.main()
