#!/usr/bin/env python3
"""Ordinary Cortex-M3 C code and untouched BSP readiness evidence."""

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROBE_CASES = [
    ("probe_select", 0x00000000, 0x00000009),
    ("probe_select", 0x00000001, 0x00000007),
    ("probe_negate", 0x00000080, 0xFFFFFF80),
    ("probe_negate", 0xFFFFFF80, 0x00000080),
    ("probe_compare", 0xFFFFFF80, 0x00000001),
    ("probe_compare", 0xFFFFFF81, 0x00000000),
]
RUNNER_SOURCE = r'''#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/loader.hpp"

int main(int argc, char** argv) {
    using namespace tinyarmsim;
    if (argc != 4) return 64;
    MemoryBus bus(64u * 1024u * 1024u);
    ArchitecturalState state;
    IsaInterpreter cpu(state, bus);
    std::ifstream elf(argv[1], std::ios::binary);
    Loader::load_elf(elf, bus, state, Loader::ElfLoadMode::Firmware);
    const auto entry = static_cast<uint32_t>(std::stoul(argv[2], nullptr, 0));
    const auto input = static_cast<uint32_t>(std::stoul(argv[3], nullptr, 0));
    state.set_pc(entry & ~1u);
    state.set_sp(0x30000u);
    state.set_lr(0x8001u);
    state.set_reg(0, input);
    try {
        unsigned steps = 0;
        for (; steps < 200 && state.get_pc() != 0x8000u; ++steps) cpu.step();
        std::cout << "returned=" << (state.get_pc() == 0x8000u)
                  << " actual=" << std::hex << state.get_reg(0)
                  << " sp=" << state.get_sp() << " steps=" << std::dec << steps << '\n';
        return state.get_pc() == 0x8000u ? 0 : 3;
    } catch (const std::exception& error) {
        const auto pc = state.get_pc();
        std::cout << "fault_pc=" << std::hex << pc << " raw1=" << bus.read16(pc)
                  << " raw2=" << bus.read16(pc + 2) << " message=" << error.what() << '\n';
        return 2;
    }
}
'''

FIRMWARE_RUNNER_SOURCE = r'''#include <fstream>
#include <iostream>
#include <string>
#include "tinyarmsim/interpreter.hpp"
#include "tinyarmsim/loader.hpp"
#include "tinyarmsim/mmio/soc_platform.hpp"

int main(int argc, char** argv) {
    using namespace tinyarmsim;
    if (argc != 3) return 64;
    MemoryBus bus(64u * 1024u * 1024u);
    ArchitecturalState state;
    IsaInterpreter cpu(state, bus);
    auto soc = mmio::SoCPeripherals::attach_all(bus, cpu);
    soc.nvic->set_vtor(0x10000u);
    std::ifstream elf(argv[1], std::ios::binary);
    Loader::load_elf(elf, bus, state, Loader::ElfLoadMode::Firmware);
    Loader::boot_cortex_m(bus, state, 0x10000u);
    state.set_reg(0, static_cast<uint32_t>(std::stoul(argv[2], nullptr, 0)));
    try {
        const auto result = cpu.run(50000);
        std::cout << "exit=" << result << " ticks=" << bus.read32(0x20010u)
                  << " uart=" << soc.uart->get_tx_output() << '\n';
        return result == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        const auto pc = state.get_pc();
        std::cout << "fault_pc=" << std::hex << pc << " raw1=" << bus.read16(pc)
                  << " raw2=" << bus.read16(pc + 2) << " message=" << error.what()
                  << " ticks=" << bus.read32(0x20010u)
                  << " uart=" << soc.uart->get_tx_output() << '\n';
        return 2;
    }
}
'''


class NaturalFirmwareTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.simulator, cls.compiler, source_root = sys.argv[1:4]
        cls.root = Path(source_root)
        cls.nm = str(Path(cls.compiler).with_name("arm-none-eabi-nm"))

    def run_checked(self, command, *, timeout=60):
        result = subprocess.run(command, text=True, capture_output=True, timeout=timeout)
        self.assertEqual(result.returncode, 0, f"command: {command}\n{result.stdout}\n{result.stderr}")
        return result

    def compile_firmware(self, source, output, optimization, extra_sources=(), keep_sections=False):
        command = [
            self.compiler,
            "-mcpu=cortex-m3",
            "-mthumb",
            optimization,
            "-ffreestanding",
            "-fno-builtin",
            "-fdata-sections",
            "-ffunction-sections",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-nostdlib",
            "-Wl,--fatal-warnings",
            "-T",
            str(self.root / "platforms/common/bsp/linker.ld"),
            str(self.root / "platforms/common/bsp/startup.s"),
            "-I", str(self.root / "platforms/common/bsp"),
            "-I", str(self.root / "platforms/02_basic_mcu/bsp"),
            *map(str, extra_sources),
            str(source),
            "-o",
            str(output),
        ]
        if not keep_sections:
            command.insert(command.index("-Wl,--fatal-warnings"), "-Wl,--gc-sections")
        return self.run_checked(command)

    def symbols(self, elf):
        output = self.run_checked([self.nm, "-n", "-S", str(elf)]).stdout
        return {
            match.group(3): {"address": int(match.group(1), 16), "size": int(match.group(2), 16)}
            for line in output.splitlines()
            if (match := re.match(r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+\w\s+(\S+)$", line))
        }

    def test_natural_c_functions_at_o0_and_o2(self):
        rows = []
        with tempfile.TemporaryDirectory(prefix="natural-fw-") as directory:
            work = Path(directory)
            runner_source = work / "probe_runner.cpp"
            runner_source.write_text(RUNNER_SOURCE)
            runner = work / "probe_runner"
            self.run_checked(["/usr/bin/g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                              "-I", str(self.root / "include"), str(runner_source), "-o", str(runner)])
            for optimization in ("-O0", "-O2"):
                elf = work / f"readiness-{optimization[1:]}.elf"
                self.compile_firmware(self.root / "platforms/02_basic_mcu/firmware/apps/readiness_probe.c", elf, optimization,
                                      keep_sections=True)
                symbol_table = self.symbols(elf)
                for function, input_value, expected in PROBE_CASES:
                    self.assertIn(function, symbol_table, f"{optimization} omitted {function}")
                    function_symbol = symbol_table[function]
                    process = subprocess.run(
                        [str(runner), str(elf), hex(function_symbol["address"]), hex(input_value)],
                        text=True,
                        capture_output=True,
                        timeout=15,
                    )
                    output = process.stdout.strip()
                    actual_match = re.search(r"actual=([0-9a-fA-F]+)", output)
                    actual = int(actual_match.group(1), 16) if actual_match and "returned=1" in output else None
                    rows.append({
                        "optimization": optimization,
                        "function": function,
                        "address": f"0x{function_symbol['address']:08x}",
                        "symbol_size": function_symbol["size"],
                        "input": f"0x{input_value:08x}",
                        "expected": f"0x{expected:08x}",
                        "actual": None if actual is None else f"0x{actual:08x}",
                        "passed": process.returncode == 0 and actual == expected,
                        "returncode": process.returncode,
                        "output": output,
                    })
                    self.__class__.probe_rows = rows
                    self.assertTrue(output, f"empty runner output: {optimization} {function} {input_value:#x}")
                    self.assertRegex(output, r"returned=1|fault_pc=0x?[0-9a-fA-F]+",
                                     f"function neither returned nor faulted: {output}")
        self.assertEqual(len(rows), 12)
        self.assertTrue(all(row["address"] for row in rows))
        for row in rows:
            self.assertTrue(row["passed"], f"natural firmware result mismatch: {row}")
        self.__class__.probe_rows = rows

    def test_untouched_demo_through_public_cli(self):
        with tempfile.TemporaryDirectory(prefix="natural-fw-demo-") as directory:
            work = Path(directory)
            elf = work / "demo_all_peripherals.elf"
            self.compile_firmware(
                self.root / "platforms/02_basic_mcu/firmware/apps/demo_all_peripherals.c", elf, "-O2",
                extra_sources=sorted((self.root / "platforms/common/bsp").glob("*.c")),
            )
            result = subprocess.run(
                [self.simulator, "--firmware", "--max-steps", "200000", str(elf)],
                text=True,
                capture_output=True,
                timeout=60,
            )
            combined = result.stdout + result.stderr
            self.assertIn("START_DEMO", combined)
            self.assertIn("ALL_PERIPHERALS_OK", combined)
            if "Undefined instruction" in combined:
                self.fail(f"demo reached unsupported instruction instead of its timer event loop:\n{combined}")
            self.assertEqual(result.returncode, 0, combined)
            self.assertIn("SIMULATION PASSED", combined)
            self.assertIn("Exit Code:          0 (R0 = 0)", combined)
            runner_source = work / "firmware_runner.cpp"
            runner_source.write_text(FIRMWARE_RUNNER_SOURCE)
            runner = work / "firmware_runner"
            self.run_checked(["/usr/bin/g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                              "-I", str(self.root / "include"), str(runner_source), "-o", str(runner)])
            irq_result = self.run_checked([str(runner), str(elf), "0"])
            irq_output = irq_result.stdout.strip()
            self.assertRegex(irq_output, r"exit=0 ticks=[1-9][0-9]*")
            tick_count = int(re.search(r"ticks=([0-9]+)", irq_output).group(1))
            self.assertIn("ALL_PERIPHERALS_OK", irq_output)
            self.__class__.demo_result = {
                "command": [self.simulator, "--firmware", "--max-steps", "200000", str(elf)],
                "returncode": result.returncode,
                "uart_start": "START_DEMO" in combined,
                "uart_complete": "ALL_PERIPHERALS_OK" in combined,
                "timer_irq": True,
                "timer_ticks": tick_count,
                "event_runner_command": [str(runner), str(elf), "0"],
                "event_runner_output": irq_output,
                "termination": "svc_exit",
                "simulator_output": combined,
            }

    @classmethod
    def tearDownClass(cls):
        report_dir = cls.root / ".reports/natural-firmware-gate"
        report_dir.mkdir(parents=True, exist_ok=True)
        rows = getattr(cls, "probe_rows", [])
        demo = getattr(cls, "demo_result", {"status": "not_completed"})
        (report_dir / "probe_results.json").write_text(json.dumps({
            "task": "TASK-063",
            "compiler": cls.compiler,
            "compiler_version": subprocess.run([cls.compiler, "--version"], text=True,
                                                   capture_output=True, check=True).stdout.splitlines()[0],
            "flags": ["-mcpu=cortex-m3", "-mthumb", "-O0/-O2", "-ffreestanding", "-fno-builtin"],
            "probe_source": str(cls.root / "platforms/02_basic_mcu/firmware/apps/readiness_probe.c"),
            "independent_literals": True,
            "semantic_mismatches": [
                row for row in rows if not row["passed"]
            ],
            "rows": rows,
            "demo": demo,
        }, indent=2) + "\n")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
