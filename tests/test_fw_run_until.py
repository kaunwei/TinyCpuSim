import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwRunUntil(unittest.TestCase):
    def test_immediate_true_returns_zero_steps_and_result_zero(self):
        source = r"""
#include <iostream>
#include "tinyarmsim/cpu/interpreter.hpp"

int main() {
    tinyarmsim::MemoryBus bus;
    tinyarmsim::ArchitecturalState state;
    tinyarmsim::IsaInterpreter interp(state, bus);

    bus.write16(0x0, 0xE7FE);
    state.set_pc(0x0);

    auto stop_cb = []() -> bool {
        return true;
    };

    uint32_t ret = interp.run_until(stop_cb, 0);
    if (ret == 0 && interp.get_stats().instruction_count == 0) {
        std::cout << "OK_IMMEDIATE_TRUE" << std::endl;
        return 0;
    }
    return 1;
}
"""
        res = run_cpp(source, timeout=10)
        self.assertEqual(res.returncode, 0)
        self.assertIn("OK_IMMEDIATE_TRUE", res.stdout)

    def test_predicate_false_three_times_then_true_returns_three_steps_and_result_zero(self):
        source = r"""
#include <iostream>
#include "tinyarmsim/cpu/interpreter.hpp"

int main() {
    tinyarmsim::MemoryBus bus;
    tinyarmsim::ArchitecturalState state;
    tinyarmsim::IsaInterpreter interp(state, bus);

    bus.write16(0x0, 0xE7FE);
    state.set_pc(0x0);

    int count = 0;
    auto stop_cb = [&count]() -> bool {
        if (count == 3) {
            return true;
        }
        count++;
        return false;
    };

    uint32_t ret = interp.run_until(stop_cb, 0);
    if (ret == 0 && interp.get_stats().instruction_count == 3) {
        std::cout << "OK_STOP_AFTER_3" << std::endl;
        return 0;
    }
    return 1;
}
"""
        res = run_cpp(source, timeout=10)
        self.assertEqual(res.returncode, 0)
        self.assertIn("OK_STOP_AFTER_3", res.stdout)

    def test_false_predicate_with_budget_two_triggers_max_step_fault(self):
        source = r"""
#include <iostream>
#include "tinyarmsim/cpu/interpreter.hpp"

int main() {
    tinyarmsim::MemoryBus bus;
    tinyarmsim::ArchitecturalState state;
    tinyarmsim::IsaInterpreter interp(state, bus);

    bus.write16(0x0, 0xE7FE);
    state.set_pc(0x0);

    auto stop_cb = []() -> bool {
        return false;
    };

    try {
        interp.run_until(stop_cb, 2);
        return 1;
    } catch (const tinyarmsim::CpuFaultException& e) {
        if (e.get_fault_type() == tinyarmsim::FaultType::MemoryOutOfBounds &&
            interp.get_stats().instruction_count == 2) {
            std::cout << "OK_BUDGET_FAULT" << std::endl;
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
        self.assertIn("OK_BUDGET_FAULT", res.stdout)


if __name__ == "__main__":
    unittest.main()
