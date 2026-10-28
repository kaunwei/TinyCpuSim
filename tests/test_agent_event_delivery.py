"""Integration tests for agent event delivery and sentinel review handoff."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE_NIGHTSHIFT = Path(__file__).resolve().parent.parent / "nightshift"


class TestAgentEventDelivery(unittest.TestCase):
    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory(prefix="ns-event-test-")
        self.root = Path(self.tmp_dir.name)
        self.ns = self.root / "nightshift"
        self.ns.mkdir(parents=True)
        for name in ["watch_worker.sh", "worker_runtime.py", "scratch_review.py"]:
            shutil.copy2(SOURCE_NIGHTSHIFT / name, self.ns / name)
        (self.ns / "watch_worker.sh").chmod(0o755)

        self.tasks_file = self.ns / "tasks.txt"
        self.tasks_done_file = self.ns / "tasks.done"
        self.progress_file = self.ns / "progress.log"
        self.status_file = self.ns / "worker.status"
        self.events_file = self.ns / ".worker-agent-events.jsonl"
        self.cursor_file = self.ns / ".worker-agent-events.cursor.json"
        self.ack_file = self.ns / ".worker-agent-events.acks.jsonl"
        self.gate_file = self.ns / ".worker-dispatch-gate.json"

        self.tasks_file.write_text("TASK-UNIT-01 | LEVEL: 1\n", encoding="utf-8")
        self.tasks_done_file.write_text("", encoding="utf-8")
        self.progress_file.write_text("", encoding="utf-8")
        self.status_file.write_text(json.dumps({"state": "IDLE", "current_task": "", "pid": 0}), encoding="utf-8")

        self.clean_env = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": os.environ.get("HOME", "/tmp"),
            "TMPDIR": str(self.root),
        }

    def tearDown(self):
        self.tmp_dir.cleanup()

    def run_watcher(self, timeout_sec="1", interval_sec="0.03", task_id="TASK-UNIT-01"):
        return subprocess.run(
            ["bash", str(self.ns / "watch_worker.sh"), str(timeout_sec), str(interval_sec), str(task_id)],
            capture_output=True,
            text=True,
            timeout=10,
            env=self.clean_env,
            cwd=str(self.root),
        )

    def run_runtime(self, *args):
        return subprocess.run(
            ["/usr/bin/python3", str(self.ns / "worker_runtime.py"), *args],
            capture_output=True,
            text=True,
            timeout=10,
            env=self.clean_env,
            cwd=str(self.root),
        )

    def test_codex_to_agy_first_delivery(self):
        """Covers Codex->AgY first delivery: watch_worker emits SENTINEL_PLANNER_REVIEW_REQUIRED and exits code 4."""
        self.status_file.write_text(
            json.dumps({"state": "PAUSED_FOR_PLANNER", "current_task": "TASK-UNIT-01", "pid": 0}),
            encoding="utf-8"
        )
        event = {
            "event_id": "ev-failover-1",
            "cause": "agent_failover",
            "task_id": "TASK-UNIT-01",
            "registration_id": "agy",
            "previous_registration_id": "codex",
            "current_registration_id": "agy",
            "previous_score": 5,
            "current_score": 5,
            "previous_level": 1,
            "current_level": 1
        }
        self.events_file.write_text(json.dumps(event) + "\n", encoding="utf-8")
        self.gate_file.write_text(json.dumps({"version": 1, "pending_event_ids": [event["event_id"]]}), encoding="utf-8")

        proc = self.run_watcher(timeout_sec="1", interval_sec="0.03", task_id="TASK-UNIT-01")
        self.assertEqual(proc.returncode, 4, f"expected code 4, got {proc.returncode}. Output: {proc.stdout}\n{proc.stderr}")
        self.assertIn("[SENTINEL_PLANNER_REVIEW_REQUIRED]", proc.stdout)
        self.assertIn("ev-failover-1", proc.stdout)
        self.assertIn("TASK-UNIT-01", proc.stdout)

        # Durable handoff line written
        handoff = self.progress_file.read_text(encoding="utf-8")
        self.assertIn("[PLANNER_REVIEW_REQUIRED]", handoff)
        self.assertIn("ev-failover-1", handoff)

        # Gate remains pending
        self.assertTrue(self.gate_file.exists())

    def test_restart_previously_seen_unacked_with_replay_pending(self):
        """Covers restarting watcher when cursor already saw the event, unacked pending event still delivered."""
        event = {
            "event_id": "ev-seen-unacked-2",
            "cause": "agent_failover",
            "task_id": "TASK-UNIT-01",
            "registration_id": "agy",
            "previous_registration_id": "codex",
            "current_registration_id": "agy",
        }
        self.events_file.write_text(json.dumps(event) + "\n", encoding="utf-8")
        # Pre-populate cursor saying it has been seen
        self.cursor_file.write_text(json.dumps({"version": 1, "seen_event_ids": ["ev-seen-unacked-2"]}), encoding="utf-8")
        # Pre-populate handoff line once
        initial_handoff = "[PLANNER_REVIEW_REQUIRED] Event=ev-seen-unacked-2 Task=TASK-UNIT-01\n"
        self.progress_file.write_text(initial_handoff, encoding="utf-8")
        self.status_file.write_text(json.dumps({"state": "PAUSED_FOR_PLANNER", "current_task": "TASK-UNIT-01", "pid": 0}), encoding="utf-8")

        # Direct CLI invocation with --replay-pending should output the event
        cli_proc = self.run_runtime(
            "agent-events-watch",
            "--events", str(self.events_file),
            "--cursor", str(self.cursor_file),
            "--acknowledgements", str(self.ack_file),
            "--gate", str(self.gate_file),
            "--handoff", str(self.progress_file),
            "--replay-pending"
        )
        self.assertEqual(cli_proc.returncode, 0)
        self.assertIn("[PLANNER_REVIEW_REQUIRED]", cli_proc.stdout)
        self.assertIn("ev-seen-unacked-2", cli_proc.stdout)

        # Ensure durable handoff line stays once (not duplicated)
        lines = [l for l in self.progress_file.read_text(encoding="utf-8").splitlines() if "ev-seen-unacked-2" in l]
        self.assertEqual(len(lines), 1)

        # Now test watcher delivers it and exits 4 promptly
        proc = self.run_watcher(timeout_sec="1", interval_sec="0.03", task_id="TASK-UNIT-01")
        self.assertEqual(proc.returncode, 4)
        self.assertIn("[SENTINEL_PLANNER_REVIEW_REQUIRED]", proc.stdout)
        self.assertIn("ev-seen-unacked-2", proc.stdout)

    def test_queued_task_status_paused_for_planner_without_done(self):
        """Covers queued task status PAUSED_FOR_PLANNER exiting code 4 promptly without needing task DONE."""
        # Task is still in tasks.txt
        self.tasks_file.write_text("TASK-UNIT-01 | LEVEL: 1\n", encoding="utf-8")
        self.status_file.write_text(
            json.dumps({"state": "PAUSED_FOR_PLANNER", "current_task": "TASK-UNIT-01", "pid": 0}),
            encoding="utf-8"
        )
        event = {
            "event_id": "ev-queued-3",
            "cause": "agent_failover",
            "task_id": "TASK-UNIT-01",
            "registration_id": "agy",
        }
        self.events_file.write_text(json.dumps(event) + "\n", encoding="utf-8")
        # No task DONE in tasks.done
        self.assertEqual(self.tasks_done_file.read_text(encoding="utf-8").strip(), "")

        proc = self.run_watcher(timeout_sec="1", interval_sec="0.03", task_id="TASK-UNIT-01")
        self.assertEqual(proc.returncode, 4)
        self.assertIn("[SENTINEL_PLANNER_REVIEW_REQUIRED]", proc.stdout)
        self.assertIn("ev-queued-3", proc.stdout)

    def test_shared_cursor_observer_isolation(self):
        """Covers shared cursor observer: second watcher or reader still sees unacked event via --replay-pending."""
        event = {
            "event_id": "ev-shared-4",
            "cause": "agent_failover",
            "task_id": "TASK-UNIT-01",
            "registration_id": "agy",
        }
        self.events_file.write_text(json.dumps(event) + "\n", encoding="utf-8")

        # First observer runs default agent-events-watch (advancing cursor)
        p1 = self.run_runtime(
            "agent-events-watch",
            "--events", str(self.events_file),
            "--cursor", str(self.cursor_file),
            "--acknowledgements", str(self.ack_file),
            "--gate", str(self.gate_file),
            "--handoff", str(self.progress_file)
        )
        self.assertEqual(p1.returncode, 0)
        self.assertIn("ev-shared-4", p1.stdout)

        # Default second run without --replay-pending outputs nothing
        p2 = self.run_runtime(
            "agent-events-watch",
            "--events", str(self.events_file),
            "--cursor", str(self.cursor_file),
            "--acknowledgements", str(self.ack_file),
            "--gate", str(self.gate_file),
            "--handoff", str(self.progress_file)
        )
        self.assertEqual(p2.returncode, 0)
        self.assertEqual(p2.stdout.strip(), "")

        # Second observer with --replay-pending still sees the unacked event
        p3 = self.run_runtime(
            "agent-events-watch",
            "--events", str(self.events_file),
            "--cursor", str(self.cursor_file),
            "--acknowledgements", str(self.ack_file),
            "--gate", str(self.gate_file),
            "--handoff", str(self.progress_file),
            "--replay-pending"
        )
        self.assertEqual(p3.returncode, 0)
        self.assertIn("ev-shared-4", p3.stdout)

    def test_multiple_pending_individually_identified(self):
        """Covers multiple pending events individually identified."""
        ev1 = {"event_id": "ev-mult-1", "cause": "agent_failover", "task_id": "TASK-UNIT-01", "registration_id": "agy"}
        ev2 = {"event_id": "ev-mult-2", "cause": "agent_unavailable", "task_id": "TASK-UNIT-02", "registration_id": "agy"}
        self.events_file.write_text(json.dumps(ev1) + "\n" + json.dumps(ev2) + "\n", encoding="utf-8")

        # Run watcher expecting no specific task (or across tasks)
        self.status_file.write_text(json.dumps({"state": "PAUSED_FOR_PLANNER", "current_task": "", "pid": 0}), encoding="utf-8")
        proc = self.run_watcher(timeout_sec="1", interval_sec="0.03", task_id="")
        self.assertEqual(proc.returncode, 4)
        self.assertIn("ev-mult-1", proc.stdout)
        self.assertIn("ev-mult-2", proc.stdout)

    def test_ack_then_quiet_and_gate_clears(self):
        """Covers ACK then quiet: once acknowledged, event is silent and gate clears."""
        event = {
            "event_id": "ev-ack-5",
            "cause": "agent_failover",
            "task_id": "TASK-UNIT-01",
            "registration_id": "agy",
        }
        self.events_file.write_text(json.dumps(event) + "\n", encoding="utf-8")

        # Initially gated
        self.run_runtime(
            "agent-events-watch",
            "--events", str(self.events_file),
            "--cursor", str(self.cursor_file),
            "--acknowledgements", str(self.ack_file),
            "--gate", str(self.gate_file),
            "--handoff", str(self.progress_file)
        )
        self.assertTrue(self.gate_file.exists())

        # Acknowledge the event
        ack_res = self.run_runtime(
            "agent-event-ack",
            "--events", str(self.events_file),
            "--acknowledgements", str(self.ack_file),
            "--event-id", "ev-ack-5",
            "--resolution", "acknowledged"
        )
        self.assertEqual(ack_res.returncode, 0)

        # Watch again with --replay-pending
        watch_res = self.run_runtime(
            "agent-events-watch",
            "--events", str(self.events_file),
            "--cursor", str(self.cursor_file),
            "--acknowledgements", str(self.ack_file),
            "--gate", str(self.gate_file),
            "--handoff", str(self.progress_file),
            "--replay-pending"
        )
        self.assertEqual(watch_res.returncode, 0)
        self.assertEqual(watch_res.stdout.strip(), "")
        # Gate file must be cleared
        self.assertFalse(self.gate_file.exists())

        # Watcher runs without review trigger, reaches timeout (or complete if done)
        proc = self.run_watcher(timeout_sec="1", interval_sec="0.03", task_id="TASK-UNIT-01")
        self.assertNotEqual(proc.returncode, 4)
        self.assertNotIn("[SENTINEL_PLANNER_REVIEW_REQUIRED]", proc.stdout)

    def test_genuine_completion_and_delayed_audit_unaffected(self):
        """Covers genuine completion with mechanical audit PASS unaffected."""
        # Baseline audits snapshot is taken at watcher startup.
        # If an audit arrives during execution, watcher recognizes it.
        self.tasks_file.write_text("", encoding="utf-8")
        self.status_file.write_text(json.dumps({"state": "IDLE", "current_task": "", "pid": 0}), encoding="utf-8")

        # Launch watcher in background with 3s timeout
        watcher_proc = subprocess.Popen(
            ["bash", str(self.ns / "watch_worker.sh"), "3", "0.03", "TASK-UNIT-01"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=self.clean_env,
            cwd=str(self.root),
        )

        import time
        time.sleep(0.1)

        # Append done and fresh audit while watcher is running
        audit_line = "[MECHANICAL_AUDIT] Status: PASS | Task: TASK-UNIT-01 | Attempts: 1 | Tests_Run: 5 | Duration: 1s\n"
        done_line = "[DONE] TASK-UNIT-01\n"
        with self.progress_file.open("a", encoding="utf-8") as f:
            f.write(audit_line)
        with self.tasks_done_file.open("a", encoding="utf-8") as f:
            f.write(done_line)

        stdout, stderr = watcher_proc.communicate(timeout=5)
        self.assertEqual(watcher_proc.returncode, 0, f"STDOUT: {stdout}\nSTDERR: {stderr}")
        self.assertIn("[SENTINEL_COMPLETED]", stdout)


if __name__ == "__main__":
    unittest.main()
