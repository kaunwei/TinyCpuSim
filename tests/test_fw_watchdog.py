"""Tests for target-service progress watchdog and success predicates (Slice 17)."""

import pathlib
import sys
import unittest

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from scripts.firmware_test_harness import run_service_watchdog, FirmwareTimeoutError


class TestFirmwareWatchdog(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sim = _REPO_ROOT / "build" / "tinyarmsim"
        cls.elf = _REPO_ROOT / "build" / "scheduler_acceptance.elf"
        if not cls.sim.exists() or not cls.elf.exists():
            raise unittest.SkipTest("tinyarmsim binary or scheduler_acceptance.elf missing")

    def test_watchdog_detects_target_service_success(self):
        res = run_service_watchdog(
            str(self.sim),
            str(self.elf),
            target_service_predicate=lambda out: "SCHEDULER_ACCEPTANCE_PASS" in out,
            step_budget=500000,
            timeout_sec=5.0,
        )
        self.assertEqual(res.returncode, 0)
        self.assertIn("SCHED_START", res.stdout)
        self.assertIn("SCHEDULER_ACCEPTANCE_PASS", res.stdout)

    def test_watchdog_does_not_confuse_heartbeat_with_service_health(self):
        with self.assertRaises(FirmwareTimeoutError) as ctx:
            run_service_watchdog(
                str(self.sim),
                str(self.elf),
                target_service_predicate=lambda out: "MISSING_SERVICE_OUTPUT" in out,
                step_budget=500000,
                timeout_sec=1.5,
            )
        err_msg = str(ctx.exception)
        self.assertIn("Target service predicate not satisfied", err_msg)
        # Verify that despite heartbeats being observed, the watchdog did NOT succeed
        self.assertIn("Heartbeats observed", err_msg)

    def test_watchdog_early_failure_on_small_step_budget(self):
        with self.assertRaises(FirmwareTimeoutError):
            run_service_watchdog(
                str(self.sim),
                str(self.elf),
                target_service_predicate=lambda out: "SCHEDULER_ACCEPTANCE_PASS" in out,
                step_budget=500,  # 500 steps is too small to complete
                timeout_sec=2.0,
            )


if __name__ == "__main__":
    unittest.main()
