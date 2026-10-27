import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwRunLoop(unittest.TestCase):
    def test_branch_to_self_positive_limit_preserves_fault_and_count(self):
        source = r"""
#include <iostream>
#include "tinyarmsim/interpreter.hpp"

int main() {
    tinyarmsim::MemoryBus bus;
    tinyarmsim::ArchitecturalState state;
    tinyarmsim::IsaInterpreter interp(state, bus);

    // B . (0xE7FE is branch-to-self Thumb instruction: offset -2 halfwords => infinite loop)
    bus.write16(0x0, 0xE7FE);
    state.set_pc(0x0);

    try {
        interp.run(10);
        return 1; // Should not reach here
    } catch (const tinyarmsim::CpuFaultException& e) {
        if (e.get_fault_type() == tinyarmsim::FaultType::MemoryOutOfBounds &&
            interp.get_stats().instruction_count == 10) {
            std::cout << "OK_STEP_LIMIT" << std::endl;
            return 0;
        }
        return 2;
    } catch (...) {
        return 3;
    }
}
"""
        res = run_cpp(source, timeout=10)
        self.assertEqual(res.returncode, 0)
        self.assertIn("OK_STEP_LIMIT", res.stdout)

    def test_run0_preserves_fault(self):
        source = r"""
#include <iostream>
#include "tinyarmsim/interpreter.hpp"

int main() {
    tinyarmsim::MemoryBus bus;
    tinyarmsim::ArchitecturalState state;
    tinyarmsim::IsaInterpreter interp(state, bus);

    bus.write16(0x0, 0xE7FE);
    state.set_pc(0x0);

    try {
        interp.run(0);
        return 1;
    } catch (const tinyarmsim::CpuFaultException& e) {
        if (e.get_fault_type() == tinyarmsim::FaultType::MemoryOutOfBounds &&
            interp.get_stats().instruction_count == 0) {
            std::cout << "OK_RUN_0" << std::endl;
            return 0;
        }
        return 2;
    } catch (...) {
        return 3;
    }
}
"""
        res = run_cpp(source, timeout=10)
        self.assertEqual(res.returncode, 0)
        self.assertIn("OK_RUN_0", res.stdout)

    def test_svc_returns_preset_r0(self):
        source = r"""
#include <iostream>
#include "tinyarmsim/interpreter.hpp"

int main() {
    tinyarmsim::MemoryBus bus;
    tinyarmsim::ArchitecturalState state;
    tinyarmsim::IsaInterpreter interp(state, bus);

    // SVC #0 (Thumb: 0xDF00)
    bus.write16(0x0, 0xDF00);
    state.set_pc(0x0);
    state.set_reg(0, 42);

    try {
        uint32_t ret = interp.run(10);
        if (ret == 42) {
            std::cout << "OK_SVC" << std::endl;
            return 0;
        }
        return 1;
    } catch (...) {
        return 2;
    }
}
"""
        res = run_cpp(source, timeout=10)
        self.assertEqual(res.returncode, 0)
        self.assertIn("OK_SVC", res.stdout)


if __name__ == "__main__":
    unittest.main()
