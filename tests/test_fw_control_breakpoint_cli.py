import pathlib
import subprocess
import unittest

from tests.fw_control_test_support import controlled_firmware


_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


class TestFwControlBreakpointCli(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        elf = _REPO_ROOT / "build" / "isa_boot_acceptance.elf"
        if not elf.exists():
            raise FileNotFoundError(f"Missing required firmware binary: {elf}")
        subprocess.run(
            ["cmake", "--build", "build", "--target", "tinycpusim", "-j2"],
            cwd=str(_REPO_ROOT),
            check=True,
            capture_output=True,
            text=True,
            timeout=120,
        )

    def test_initial_breakpoint_step_and_stop(self):
        with controlled_firmware(initial_commands="break 0x10080\n") as session:
            configured = session.receive()
            self.assertEqual(configured, {"event": "breakpoint_set", "pc": 65664})

            hit = session.receive()
            self.assertEqual(hit, {"event": "breakpoint", "pc": 65664})

            session.send("regs\n")
            before = session.receive()
            self.assertEqual(before.get("event"), "regs")
            self.assertEqual(before.get("pc"), 0x10080)
            self.assertEqual(before.get("steps"), 0)

            session.send("step\nregs\n")
            self.assertEqual(session.receive(), {"event": "stepped"})
            after = session.receive()
            self.assertEqual(after.get("event"), "regs")
            self.assertEqual(after.get("steps"), 1)
            self.assertEqual(after.get("pc"), 0x10084)

            session.send("stop\n")
            self.assertEqual(
                session.receive(), {"event": "stopped", "reason": "requested"}
            )
            session.communicate(timeout=2.0)
            self.assertEqual(session.process.returncode, 0)


if __name__ == "__main__":
    unittest.main()
