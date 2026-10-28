import pathlib
import subprocess
import sys
import unittest

from scripts.firmware_test_harness import FirmwareTimeoutError, run_bounded
from tests.fw_control_test_support import controlled_firmware


class TestFwControlDeadline(unittest.TestCase):
    def test_stop_before_deadline_succeeds(self):
        with controlled_firmware(initial_commands="stop\n", host_timeout=2.0) as session:
            self.assertEqual(session.receive(), {"event": "stopped", "reason": "requested"})
            session.communicate(timeout=1.0)
            self.assertEqual(session.process.returncode, 0)

    def test_idle_paused_session_expires_and_reaps_child(self):
        inner_check = """\
from tests.fw_control_test_support import controlled_firmware

proc_ref = None
paused_acknowledged = False
try:
    with controlled_firmware(initial_commands="pause\\n", host_timeout=0.2) as session:
        proc_ref = session.process
        assert session.receive() == {"event": "paused"}
        paused_acknowledged = True
        # Leave the socket idle so the real host deadline expires.
        session.sock.recv(1)
except TimeoutError as error:
    assert paused_acknowledged, "host deadline expired before pause acknowledgement"
    assert "host deadline" in str(error), str(error)
else:
    raise AssertionError("paused firmware session did not raise its host deadline")

assert proc_ref is not None
assert proc_ref.poll() is not None, "deadline did not reap the simulator child"
"""
        command = [sys.executable, "-c", inner_check]
        try:
            result = run_bounded(command, timeout=3.0)
        except FirmwareTimeoutError as error:
            self.fail(f"outer deadline expired instead of completing checks: {error}")
        self.assertEqual(
            result.returncode,
            0,
            f"inner deadline check failed\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )


if __name__ == "__main__":
    unittest.main()
