import unittest

from tests.fw_control_test_support import controlled_firmware


class TestFwControlMemoryCli(unittest.TestCase):
    def test_cli_reads_ram_and_invalid_request_keeps_paused_simulation_alive(self):
        with controlled_firmware(initial_commands="pause\n") as session:
            self.assertEqual(session.receive(), {"event": "paused"})
            session.send("mem 0x10000 8\n")
            memory = session.receive()
            self.assertEqual(memory, {
                "event": "memory",
                "address": 0x10000,
                "bytes": [0x00, 0x00, 0x03, 0x00, 0x81, 0x00, 0x01, 0x00],
            })

            session.send("regs\n")
            regs_before = session.receive()
            self.assertEqual(regs_before["event"], "regs")

            session.send("mem 0x40000000 4\n")
            self.assertEqual(session.receive(), {
                "error": "memory access out of bounds or mmio rejected"
            })
            self.assertIsNone(session.proc.poll())

            session.send("regs\n")
            regs_after = session.receive()
            self.assertEqual(regs_after, regs_before)

            session.send("stop\n")
            self.assertEqual(session.receive(), {"event": "stopped", "reason": "requested"})
            stdout, stderr = session.communicate(timeout=5)
            self.assertEqual(session.proc.returncode, 0, f"stdout={stdout}\nstderr={stderr}")


if __name__ == "__main__":
    unittest.main()
