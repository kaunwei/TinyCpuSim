import pathlib
import subprocess
import unittest
from tests.fw_control_test_support import controlled_firmware

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


class TestFwControlStepCli(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sim = _REPO_ROOT / "build" / "tinycpusim"
        elf = _REPO_ROOT / "build" / "isa_boot_acceptance.elf"
        if not elf.exists():
            raise FileNotFoundError(f"Missing required firmware binary: {elf}")
        subprocess.run(
            ["cmake", "--build", "build", "--target", "tinycpusim", "-j2"],
            cwd=str(_REPO_ROOT),
            check=True,
            capture_output=True,
            text=True,
        )
        if not sim.exists():
            raise FileNotFoundError(f"Missing built simulator binary: {sim}")

    def test_real_cli_step_behavior(self):
        with controlled_firmware(initial_commands="pause\nregs\n") as session:
            paused_msg = session.receive()
            self.assertEqual(paused_msg, {"event": "paused"})

            init_regs = session.receive()
            self.assertEqual(init_regs.get("event"), "regs")
            self.assertEqual(init_regs.get("steps"), 0)
            init_pc = init_regs.get("pc")
            self.assertIsInstance(init_pc, int)

            session.send("step\nregs\n")
            stepped_msg = session.receive()
            self.assertEqual(stepped_msg, {"event": "stepped"})

            step_regs = session.receive()
            self.assertEqual(step_regs.get("event"), "regs")
            self.assertEqual(step_regs.get("steps"), 1)
            self.assertEqual(step_regs.get("pc"), init_pc + 4)

            session.send("regs\n")
            paused_again_regs = session.receive()
            self.assertEqual(paused_again_regs, step_regs)

            session.send("stop\n")
            stop_ack = session.receive()
            self.assertEqual(stop_ack, {"event": "stopped", "reason": "requested"})

            session.communicate(timeout=2.0)
            self.assertEqual(session.process.returncode, 0)


if __name__ == "__main__":
    unittest.main()
