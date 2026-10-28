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

        def _cleanup_watcher():
            if watcher_proc.poll() is None:
                watcher_proc.terminate()
                try:
                    watcher_proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    watcher_proc.kill()
                    watcher_proc.wait(timeout=2)
            if watcher_proc.stdout and not watcher_proc.stdout.closed:
                watcher_proc.stdout.close()
            if watcher_proc.stderr and not watcher_proc.stderr.closed:
                watcher_proc.stderr.close()

        self.addCleanup(_cleanup_watcher)

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

    def test_two_different_watch_tasks_both_notify_exit4_and_ack_clears(self):
        """Covers: two different watch tasks both notify exit 4 despite shared cursor, restart seen unacked notify, ACK then both quiet/gate clears."""
        event = {
            "event_id": "ev-global-cross-task",
            "cause": "agent_failover",
            "task_id": "TASK-OTHER-99",
            "registration_id": "agy",
        }
        self.events_file.write_text(json.dumps(event) + "\n", encoding="utf-8")
        self.gate_file.write_text(json.dumps({"version": 1, "pending_event_ids": [event["event_id"]]}), encoding="utf-8")
        self.status_file.write_text(json.dumps({"state": "PAUSED_FOR_PLANNER", "current_task": "TASK-WATCH-A", "pid": 0}), encoding="utf-8")

        # Watcher watching TASK-WATCH-A should wake, notify and exit 4 even though event is for TASK-OTHER-99
        proc_a = self.run_watcher(timeout_sec="1", interval_sec="0.03", task_id="TASK-WATCH-A")
        self.assertEqual(proc_a.returncode, 4, f"expected 4, got {proc_a.returncode}\n{proc_a.stdout}")
        self.assertIn("[SENTINEL_PLANNER_REVIEW_REQUIRED]", proc_a.stdout)
        self.assertIn("Task=TASK-OTHER-99", proc_a.stdout)
        self.assertIn("ev-global-cross-task", proc_a.stdout)

        # Shared cursor now contains ev-global-cross-task.
        # Second watcher watching TASK-WATCH-B should also notify and exit 4 via replay of unacked event
        self.status_file.write_text(json.dumps({"state": "PAUSED_FOR_PLANNER", "current_task": "TASK-WATCH-B", "pid": 0}), encoding="utf-8")
        proc_b = self.run_watcher(timeout_sec="1", interval_sec="0.03", task_id="TASK-WATCH-B")
        self.assertEqual(proc_b.returncode, 4, f"expected 4, got {proc_b.returncode}\n{proc_b.stdout}")
        self.assertIn("[SENTINEL_PLANNER_REVIEW_REQUIRED]", proc_b.stdout)
        self.assertIn("Task=TASK-OTHER-99", proc_b.stdout)
        self.assertIn("ev-global-cross-task", proc_b.stdout)

        # Restarting watcher on seen unacked event still notifies
        proc_restart = self.run_watcher(timeout_sec="1", interval_sec="0.03", task_id="TASK-WATCH-A")
        self.assertEqual(proc_restart.returncode, 4)
        self.assertIn("[SENTINEL_PLANNER_REVIEW_REQUIRED]", proc_restart.stdout)

        # ACK the event: both become quiet and gate clears
        ack_res = self.run_runtime(
            "agent-event-ack",
            "--events", str(self.events_file),
            "--acknowledgements", str(self.ack_file),
            "--event-id", "ev-global-cross-task",
            "--resolution", "acknowledged"
        )
        self.assertEqual(ack_res.returncode, 0)

        # Trigger gate recheck via watch_agent_events
        self.run_runtime(
            "agent-events-watch",
            "--events", str(self.events_file),
            "--cursor", str(self.cursor_file),
            "--acknowledgements", str(self.ack_file),
            "--gate", str(self.gate_file),
            "--handoff", str(self.progress_file),
            "--replay-pending"
        )
        self.assertFalse(self.gate_file.exists())

        # Now neither watcher exits 4
        quiet_a = self.run_watcher(timeout_sec="1", interval_sec="0.03", task_id="TASK-WATCH-A")
        self.assertNotEqual(quiet_a.returncode, 4)
        self.assertNotIn("[SENTINEL_PLANNER_REVIEW_REQUIRED]", quiet_a.stdout)

        quiet_b = self.run_watcher(timeout_sec="1", interval_sec="0.03", task_id="TASK-WATCH-B")
        self.assertNotEqual(quiet_b.returncode, 4)
        self.assertNotIn("[SENTINEL_PLANNER_REVIEW_REQUIRED]", quiet_b.stdout)

    def test_default_cli_handoff_exists_cursor_removed_silent_vs_replay(self):
        """Covers: original existing handoff + missing cursor must not emit duplicate default CLI output; opt-in replay still notifies all unacked."""
        event = {
            "event_id": "ev-compat-6",
            "cause": "agent_failover",
            "task_id": "TASK-UNIT-01",
            "registration_id": "agy",
        }
        self.events_file.write_text(json.dumps(event) + "\n", encoding="utf-8")
        # Existing handoff in progress_file
        line = f"[PLANNER_REVIEW_REQUIRED] Event={event['event_id']} Task={event['task_id']}\n"
        self.progress_file.write_text(line, encoding="utf-8")
        # Cursor is missing / removed
        self.cursor_file.unlink(missing_ok=True)

        # Default CLI run (no --replay-pending): should be silent because event was already handed off
        p_default = self.run_runtime(
            "agent-events-watch",
            "--events", str(self.events_file),
            "--cursor", str(self.cursor_file),
            "--acknowledgements", str(self.ack_file),
            "--gate", str(self.gate_file),
            "--handoff", str(self.progress_file)
        )
        self.assertEqual(p_default.returncode, 0)
        self.assertEqual(p_default.stdout.strip(), "")

        # Opt-in replay still notifies unacked event
        p_replay = self.run_runtime(
            "agent-events-watch",
            "--events", str(self.events_file),
            "--cursor", str(self.cursor_file),
            "--acknowledgements", str(self.ack_file),
            "--gate", str(self.gate_file),
            "--handoff", str(self.progress_file),
            "--replay-pending"
        )
        self.assertEqual(p_replay.returncode, 0)
        self.assertIn("ev-compat-6", p_replay.stdout)

    def test_malformed_event_stderr_visible_gate_preserved_no_fake_completion(self):
        """Covers: stderr remains visible when CLI parse/runtime fails, plus generic warning/gate stays held, no fake completion."""
        # Write malformed JSON line in events_file
        self.events_file.write_text("{malformed json line\n", encoding="utf-8")
        self.gate_file.write_text(json.dumps({"version": 1, "pending_event_ids": ["ev-malformed"]}), encoding="utf-8")
        self.status_file.write_text(json.dumps({"state": "PAUSED_FOR_PLANNER", "current_task": "TASK-UNIT-01", "pid": 0}), encoding="utf-8")

        proc = self.run_watcher(timeout_sec="1", interval_sec="0.03", task_id="TASK-UNIT-01")
        # Stderr must contain the runtime error (ValueError / JSONDecodeError) and the sentinel warning
        self.assertIn("invalid JSON line", proc.stderr)
        self.assertIn("[SENTINEL_WARNING] Agent event watcher failed; dispatch remains gated.", proc.stderr)
        # Gate remains held
        self.assertTrue(self.gate_file.exists())
        # No fake completion or review delivery
        self.assertNotEqual(proc.returncode, 0)
        self.assertNotIn("[SENTINEL_COMPLETED]", proc.stdout)
        self.assertNotIn("[SENTINEL_PLANNER_REVIEW_REQUIRED]", proc.stdout)


if __name__ == "__main__":
    unittest.main()
