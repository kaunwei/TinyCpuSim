import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tests.fw_control_test_support import controlled_firmware


_REPO_ROOT = Path(__file__).resolve().parents[1]


def _run_tool(args, *, cwd, timeout=10):
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=timeout)


def _symbols(nm_output):
    symbols = {}
    for line in nm_output.splitlines():
        fields = line.split()
        if len(fields) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", fields[0]):
            symbols[fields[-1]] = int(fields[0], 16)
    return symbols


def _instructions(objdump_output):
    instructions = {}
    for line in objdump_output.splitlines():
        match = re.match(r"^[ \t]*([0-9a-fA-F]+):", line)
        if not match:
            continue
        columns = line.split("\t")
        if len(columns) < 3:
            continue
        encoded = columns[1].split()
        if not encoded or any(not re.fullmatch(r"(?:[0-9a-fA-F]{4}|[0-9a-fA-F]{8})", token) for token in encoded):
            continue
        address = int(match.group(1), 16)
        width = sum(len(token) for token in encoded) // 2
        if width in (2, 4):
            instructions[address] = width
    return instructions


class FirmwareDebugWalkthroughTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(
            ["cmake", "--build", "build", "--target", "tinycpusim", "-j2"],
            cwd=_REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
            timeout=120,
        )

    def _walkthrough(self, opt_flag):
        template = _REPO_ROOT / "firmware" / "template"
        nm = shutil.which("arm-none-eabi-nm") or shutil.which("nm")
        objdump = shutil.which("arm-none-eabi-objdump") or shutil.which("objdump")
        self.assertIsNotNone(nm, "Neither arm-none-eabi-nm nor nm found in PATH")
        self.assertIsNotNone(objdump, "Neither arm-none-eabi-objdump nor objdump found in PATH")

        with tempfile.TemporaryDirectory() as directory:
            copied = Path(directory) / "template"
            shutil.copytree(template, copied)
            build = _run_tool(["make", f"OPT={opt_flag}"], cwd=copied, timeout=30)
            self.assertEqual(build.returncode, 0, f"make {opt_flag} failed:\n{build.stdout}\n{build.stderr}")
            elf = copied / "template.elf"

            nm_result = _run_tool([nm, str(elf)], cwd=copied)
            self.assertEqual(nm_result.returncode, 0, f"nm failed:\n{nm_result.stderr}")
            symbols = _symbols(nm_result.stdout)
            self.assertIn("main", symbols, f"main missing from nm output:\n{nm_result.stdout}")
            self.assertIn("g_counter", symbols, f"g_counter missing from nm output:\n{nm_result.stdout}")
            main = symbols["main"] & ~1
            counter = symbols["g_counter"]

            disassembly = _run_tool([objdump, "-d", str(elf)], cwd=copied)
            self.assertEqual(disassembly.returncode, 0, f"objdump failed:\n{disassembly.stderr}")
            widths = _instructions(disassembly.stdout)
            self.assertIn(main, widths, f"main address 0x{main:x} absent from disassembly")

            with controlled_firmware(
                initial_commands=f"break 0x{main:x}\n",
                elf_path=elf,
                max_steps=1000,
                host_timeout=10,
            ) as session:
                self.assertEqual(session.receive(), {"event": "breakpoint_set", "pc": main})
                self.assertEqual(session.receive(), {"event": "breakpoint", "pc": main})

                session.send("regs\n")
                before = session.receive()
                self.assertEqual(before.get("event"), "regs")
                self.assertEqual(before.get("pc"), main)
                startup_steps = before.get("steps")
                self.assertIsInstance(startup_steps, int)
                self.assertGreater(startup_steps, 0)

                session.send(f"mem 0x{counter:x} 4\n")
                memory = session.receive()
                self.assertEqual(memory.get("event"), "memory")
                self.assertEqual(memory.get("address"), counter)
                self.assertEqual(memory.get("bytes"), [0, 0, 0, 0])

                stepped_pc = main + widths[main]
                session.send("step\nregs\n")
                self.assertEqual(session.receive(), {"event": "stepped"})
                after_step = session.receive()
                self.assertEqual(after_step.get("event"), "regs")
                self.assertEqual(after_step.get("pc"), stepped_pc)
                self.assertEqual(after_step.get("steps"), startup_steps + 1)
                self.assertIn(stepped_pc, widths)

                next_pc = next((address for address in sorted(widths) if address > stepped_pc), None)
                self.assertIsNotNone(next_pc, f"No subsequent disassembled instruction after 0x{stepped_pc:x}")
                session.send(f"break 0x{next_pc:x}\ncontinue\n")
                self.assertEqual(session.receive(), {"event": "breakpoint_set", "pc": next_pc})
                self.assertEqual(session.receive(), {"event": "continued"})
                hit = session.receive()
                self.assertEqual(hit, {"event": "breakpoint", "pc": next_pc})
                session.send("regs\n")
                at_next = session.receive()
                self.assertEqual(at_next.get("event"), "regs")
                self.assertEqual(at_next.get("pc"), next_pc)
                self.assertGreater(at_next.get("steps", 0), after_step["steps"])

                session.send("stop\n")
                self.assertEqual(session.receive(), {"event": "stopped", "reason": "requested"})
                stdout, stderr = session.communicate(timeout=2.0)
                self.assertEqual(session.process.returncode, 0, f"stdout={stdout}\nstderr={stderr}")

    def test_minimal_copied_firmware_debug_walkthrough_o0(self):
        self._walkthrough("-O0")

    def test_minimal_copied_firmware_debug_walkthrough_o2(self):
        self._walkthrough("-O2")


if __name__ == "__main__":
    unittest.main()
