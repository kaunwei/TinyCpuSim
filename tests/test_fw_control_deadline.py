import threading
import unittest

from tests.fw_control_test_support import controlled_firmware


class TestFwControlDeadline(unittest.TestCase):
    def test_stop_before_deadline_succeeds(self):
        with controlled_firmware(initial_commands="stop\n", host_timeout=2.0) as session:
            self.assertEqual(session.receive(), {"event": "stopped", "reason": "requested"})
            session.communicate(timeout=1.0)
            self.assertEqual(session.process.returncode, 0)

    def test_idle_paused_session_expires_and_reaps_child(self):
        proc_ref = None
        watchdog = threading.Timer(3.0, self.fail, ("test watchdog expired",))
        watchdog.daemon = True
        watchdog.start()
        try:
            with self.assertRaisesRegex(TimeoutError, "host deadline"):
                with controlled_firmware(initial_commands="pause\n", host_timeout=0.2) as session:
                    proc_ref = session.process
                    self.assertEqual(session.receive(), {"event": "paused"})
                    # Intentionally make no further socket calls while the host deadline expires.
                    threading.Event().wait(0.4)
            self.assertIsNotNone(proc_ref)
            self.assertIsNotNone(proc_ref.poll())
        finally:
            watchdog.cancel()


if __name__ == "__main__":
    unittest.main()
