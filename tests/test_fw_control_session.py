import subprocess
import unittest
from tests.fw_control_test_support import controlled_firmware


class TestFwControlSession(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(
            ["cmake", "--build", "build", "--target", "tinycpusim", "-j2"],
            check=True,
            capture_output=True,
            text=True,
            timeout=120,
        )

    def test_preloaded_stop_ack_and_exit_zero(self):
        with controlled_firmware(initial_commands="stop\n") as session:
            msg = session.receive()
            self.assertEqual(msg, {"event": "stopped", "reason": "requested"})
            stdout, stderr = session.communicate(timeout=2.0)
            self.assertEqual(session.process.returncode, 0)

    def test_caller_exception_cleans_up_process(self):
        proc_ref = None
        with self.assertRaises(RuntimeError):
            with controlled_firmware(initial_commands="stop\n") as session:
                proc_ref = session.process
                raise RuntimeError("caller error")
        self.assertIsNotNone(proc_ref)
        self.assertIsNotNone(proc_ref.poll())


if __name__ == "__main__":
    unittest.main()
