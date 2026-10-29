"""Completion delivery at the public watcher command, without model calls."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SentinelCompletionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.ns = self.root / 'nightshift'
        self.ns.mkdir()
        shutil.copy2(ROOT / 'nightshift/watch_worker.sh', self.ns / 'watch_worker.sh')
        for name in ('tasks.txt', 'tasks.done', 'progress.log'):
            (self.ns / name).write_text('')
        self.status('IDLE')

    def status(self, state, task='', pid=None):
        path = self.ns / 'worker.status'
        temporary = path.with_suffix('.tmp')
        temporary.write_text(json.dumps({'state': state, 'current_task': task, 'pid': os.getpid() if pid is None else pid}))
        temporary.replace(path)

    def dead_pid(self):
        dead = subprocess.Popen(['/bin/true'])
        dead.wait()
        return dead.pid

    def start(self, task='TASK-ONE', limit='3', env_overrides=None, expect_sentinel=True):
        watcher_env = os.environ.copy()
        watcher_env['WORKER_INACTIVE_REPEAT'] = '0'
        if env_overrides:
            watcher_env.update(env_overrides)
        for name, filename in {
            'TASKS_FILE': 'tasks.txt', 'TASKS_DONE_FILE': 'tasks.done',
            'STATUS_FILE': 'worker.status', 'PROGRESS_FILE': 'progress.log',
            'WORKER_AGENT_EVENTS_FILE': '.worker-agent-events.jsonl',
            'WORKER_AGENT_EVENTS_CURSOR_FILE': '.worker-agent-events.cursor.json',
            'WORKER_AGENT_EVENTS_ACK_FILE': '.worker-agent-events.acks.jsonl',
            'AGENT_DISPATCH_GATE_FILE': '.worker-dispatch-gate.json',
        }.items():
            watcher_env[name] = str(self.ns / filename)
        watcher_env['WORKER_ID'] = 'default'
        watcher_env['WATCH_TASK_ID'] = ''
        self.process = subprocess.Popen(
            ['bash', str(self.ns / 'watch_worker.sh'), limit, '0.02', task],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=watcher_env)
        self.addCleanup(self.stop)
        if expect_sentinel:
            self.assertIn('[SENTINEL]', self.process.stdout.readline())
        return self.process

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.communicate(timeout=5)

    def done(self, task='TASK-ONE', kind='DONE'):
        with (self.ns / 'tasks.done').open('a') as out:
            out.write(f'[now] [{kind}] {task} | LEVEL: 1\n')

    def audit(self, task='TASK-ONE', review='pending', audit_ns='123'):
        with (self.ns / 'progress.log').open('a') as out:
            out.write(f'[MECHANICAL_AUDIT] Status: PASS | Task: {task} | Verify_Exit: 0 | '
                      f'Tests_Run: >=1 | Whitelist: PASS | Tree: CLEAN | Audit_NS: {audit_ns} | '
                      f'Planner_Review: {review}\n')

    def assert_completed(self):
        out, err = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 0, out + err)
        self.assertIn('[SENTINEL_COMPLETED]', out)

    def test_research_tag_matches_exact_task(self):
        self.start()
        self.done('TASK-ONE [RESEARCH]')
        self.audit('TASK-ONE[RESEARCH]')
        self.assert_completed()

    def test_done_is_retained_until_delayed_audit_arrives(self):
        self.start()
        self.done()
        time.sleep(0.15)
        self.audit()
        self.assert_completed()

    def test_pending_completion_is_replayed_when_watcher_starts_late(self):
        self.done()
        self.audit()
        self.start()
        self.assert_completed()

    def test_running_agent_outlives_wait_window_without_planner_notification(self):
        self.status('RUNNING', 'TASK-ONE')
        self.start(limit='1')
        time.sleep(1.15)
        self.assertIsNone(self.process.poll(), 'live agent must not trigger a time-based wakeup')
        self.done()
        self.audit()
        self.status('IDLE')
        self.assert_completed()

    def test_result_waits_for_all_workers_executing_same_task_to_finish(self):
        self.status('RUNNING', 'TASK-ONE')
        other = self.ns / 'worker.helper.status'
        other.write_text(json.dumps({'state': 'RUNNING', 'current_task': 'TASK-ONE'}))
        self.start()
        self.done()
        self.audit()
        self.status('IDLE')
        time.sleep(0.15)
        self.assertIsNone(self.process.poll())
        other.write_text(json.dumps({'state': 'IDLE', 'current_task': ''}))
        self.assert_completed()

    def test_unrelated_failure_and_stale_blocked_do_not_end_task_watch(self):
        self.status('BLOCKED', 'TASK-OTHER')
        self.start()
        self.done('TASK-OTHER', kind='FAILED')
        time.sleep(0.15)
        self.assertIsNone(self.process.poll())
        self.done()
        self.audit()
        self.assert_completed()

    def test_reviewed_historical_completion_is_not_replayed(self):
        self.done()
        self.audit(review='accept')
        self.start(limit='1')
        out, err = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 2, out + err)
        self.assertNotIn('[SENTINEL_COMPLETED]', out)

    def test_pending_old_audit_cannot_satisfy_new_attempt_on_late_start(self):
        self.done('TASK-ONE [Audit_NS: 999]')
        self.audit(audit_ns='123')
        self.start(limit='1')
        out, err = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 2, out + err)
        self.assertNotIn('[SENTINEL_COMPLETED]', out)

    def test_matching_audit_identity_can_be_replayed_after_late_start(self):
        self.done('TASK-ONE [RESEARCH] [Audit_NS: 999]')
        self.audit('TASK-ONE[RESEARCH]', audit_ns='999')
        self.start()
        self.assert_completed()

    def test_queue_watch_replays_pending_results(self):
        self.done()
        self.audit()
        self.start(task='')
        self.assert_completed()

    def test_queue_watch_ignores_preexisting_global_blocked_status(self):
        (self.ns / 'tasks.txt').write_text('TASK-ONE | LEVEL: 1\n')
        self.status('BLOCKED', 'TASK-OLD')
        self.start(task='')
        time.sleep(0.15)
        self.assertIsNone(self.process.poll(), 'old global BLOCKED is not a fresh task event')
        self.status('RUNNING', 'TASK-ONE')
        self.done()
        self.audit()
        (self.ns / 'tasks.txt').write_text('')
        self.status('IDLE')
        self.assert_completed()


    def test_repeated_stale_blocked_transition_uses_consistent_snapshot(self):
        for iteration in range(30):
            with self.subTest(iteration=iteration):
                case = type(self)('test_queue_watch_ignores_preexisting_global_blocked_status')
                result = unittest.TestResult()
                case.run(result)
                self.assertTrue(result.wasSuccessful(), result.failures + result.errors)

    def test_genuine_current_task_blocked_still_notifies(self):
        self.status('RUNNING', 'TASK-ONE')
        self.start()
        self.status('BLOCKED', 'TASK-ONE')
        out, err = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 1, out + err)
        self.assertIn('[SENTINEL_BLOCKED]', out)

    def test_fixture_isolated_from_inherited_worker_paths(self):
        from unittest import mock
        foreign = self.root / 'foreign'
        with mock.patch.dict(os.environ, {
            'TASKS_FILE': str(foreign / 'tasks.txt'),
            'TASKS_DONE_FILE': str(foreign / 'tasks.done'),
            'STATUS_FILE': str(foreign / 'worker.status'),
            'PROGRESS_FILE': str(foreign / 'progress.log'),
            'WORKER_ID': 'foreign', 'WATCH_TASK_ID': 'TASK-FOREIGN',
        }):
            self.start()
            self.done()
            self.audit()
            self.assert_completed()

    def test_inactive_worker_triggers_sentinel_inactive(self):
        (self.ns / 'tasks.txt').write_text('TASK-ONE | LEVEL: 1\n')
        self.status('IDLE')
        self.start(limit='0', env_overrides={'WORKER_INACTIVE_TIMEOUT': '1'})
        out, err = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 5, out + err)
        self.assertIn('[SENTINEL_INACTIVE]', out)

    def test_long_running_compilation_or_test_does_not_trigger_inactive(self):
        self.status('RUNNING', 'TASK-ONE')
        self.start(limit='0', env_overrides={'WORKER_INACTIVE_TIMEOUT': '1'})
        time.sleep(1.2)
        self.assertIsNone(self.process.poll(), 'active compiling/testing worker must not trigger inactivity')
        self.done()
        self.audit()
        self.status('IDLE')
        self.assert_completed()

    def test_invalid_inactive_timeout_exits_promptly_nonzero(self):
        watcher_env = os.environ.copy()
        watcher_env['WORKER_INACTIVE_REPEAT'] = '0'
        watcher_env['WORKER_INACTIVE_TIMEOUT'] = 'invalid'
        for name, filename in {
            'TASKS_FILE': 'tasks.txt', 'TASKS_DONE_FILE': 'tasks.done',
            'STATUS_FILE': 'worker.status', 'PROGRESS_FILE': 'progress.log',
            'WORKER_AGENT_EVENTS_FILE': '.worker-agent-events.jsonl',
            'WORKER_AGENT_EVENTS_CURSOR_FILE': '.worker-agent-events.cursor.json',
            'WORKER_AGENT_EVENTS_ACK_FILE': '.worker-agent-events.acks.jsonl',
            'AGENT_DISPATCH_GATE_FILE': '.worker-dispatch-gate.json',
        }.items():
            watcher_env[name] = str(self.ns / filename)
        watcher_env['WORKER_ID'] = 'default'
        watcher_env['WATCH_TASK_ID'] = ''
        proc = subprocess.Popen(
            ['bash', str(self.ns / 'watch_worker.sh'), '0', '0.02', 'TASK-ONE'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=watcher_env)
        out, err = proc.communicate(timeout=5)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn('[SENTINEL_ERROR]', err)
        self.assertNotIn('integer expression expected', err)

    def test_dead_verifying_worker_triggers_sentinel_worker_died(self):
        self.status('VERIFYING', 'TASK-ONE', pid=self.dead_pid())
        self.start(limit='0', env_overrides={'WORKER_INACTIVE_TIMEOUT': '1'})
        out, err = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 1, out + err)
        self.assertIn('[SENTINEL_WORKER_DIED]', out)

    def test_dead_running_worker_triggers_sentinel_worker_died(self):
        self.status('RUNNING', 'TASK-ONE', pid=self.dead_pid())
        self.start(limit='0', env_overrides={'WORKER_INACTIVE_TIMEOUT': '1'})
        out, err = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 1, out + err)
        self.assertIn('[SENTINEL_WORKER_DIED]', out)

    def test_dead_unrelated_helper_does_not_suppress_inactivity_or_fail_task(self):
        (self.ns / 'tasks.txt').write_text('TASK-ONE | LEVEL: 1\n')
        self.status('IDLE')
        helper = self.ns / 'worker.helper.status'
        helper.write_text(json.dumps({'state': 'RUNNING', 'current_task': 'TASK-OLD', 'pid': self.dead_pid()}))
        self.start(limit='0', env_overrides={'WORKER_INACTIVE_TIMEOUT': '1'})
        out, err = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 5, out + err)
        self.assertIn('[SENTINEL_INACTIVE]', out)
        self.assertNotIn('[SENTINEL_FAILED]', out)
        self.assertNotIn('[SENTINEL_WORKER_DIED]', out)

    def test_live_unrelated_helper_suppresses_inactivity_without_failing_task(self):
        (self.ns / 'tasks.txt').write_text('TASK-ONE | LEVEL: 1\n')
        self.status('IDLE')
        live_proc = subprocess.Popen(['sleep', '10'])
        self.addCleanup(lambda: (live_proc.poll() is None and live_proc.terminate(), live_proc.wait()))
        helper = self.ns / 'worker.helper.status'
        helper.write_text(json.dumps({'state': 'RUNNING', 'current_task': 'TASK-OTHER', 'pid': live_proc.pid}))
        self.start(limit='0', env_overrides={'WORKER_INACTIVE_TIMEOUT': '1'})
        time.sleep(1.2)
        self.assertIsNone(self.process.poll(), 'live unrelated worker must suppress inactivity')
        live_proc.terminate()
        live_proc.wait()
        helper.write_text(json.dumps({'state': 'IDLE', 'current_task': ''}))
        out, err = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 5, out + err)
        self.assertIn('[SENTINEL_INACTIVE]', out)
        self.assertNotIn('[SENTINEL_FAILED]', out)

    def test_live_verifying_worker_does_not_trigger_inactive(self):
        self.status('VERIFYING', 'TASK-ONE')
        self.start(limit='0', env_overrides={'WORKER_INACTIVE_TIMEOUT': '1'})
        time.sleep(1.2)
        self.assertIsNone(self.process.poll(), 'active verifying worker must not trigger inactivity')
        self.done()
        self.audit()
        self.status('IDLE')
        self.assert_completed()


    def test_inactive_timeout_finite_range_rejects_huge_digits_before_comparison(self):
        for value, valid in (("9" * 128, False), ("2147483648", False), ("0", True), ("2147483647", True)):
            with self.subTest(value=value):
                proc = self.start(
                    limit='1' if valid else '0',
                    env_overrides={'WORKER_INACTIVE_TIMEOUT': value},
                    expect_sentinel=False,
                )
                try:
                    out, err = proc.communicate(timeout=5)
                    self.assertNotIn('integer expression expected', err)
                    if valid:
                        self.assertEqual(proc.returncode, 2, out + err)
                        self.assertIn('[SENTINEL_TIMEOUT]', out)
                        self.assertNotIn('[SENTINEL_ERROR]', err)
                    else:
                        self.assertEqual(proc.returncode, 1, out + err)
                        self.assertEqual(err.count('[SENTINEL_ERROR]'), 1, err)
                        self.assertIn('0..2147483647', err)
                        self.assertNotIn('[SENTINEL]', out)
                finally:
                    if proc.poll() is None:
                        proc.kill()
                    proc.communicate(timeout=3)


    def repeat_stream(self, interval='1'):
        import queue
        import threading
        stream = queue.Queue()
        self.start(limit='0', env_overrides={
            'WORKER_INACTIVE_TIMEOUT': interval, 'WORKER_INACTIVE_REPEAT': '1'})
        def read():
            for line in self.process.stdout:
                stream.put((time.monotonic(), line))
            stream.put((time.monotonic(), '<EOF>'))
        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        def finish():
            if self.process.poll() is None:
                self.process.terminate()
            self.process.wait(timeout=5)
            reader.join(timeout=5)
            self.assertFalse(reader.is_alive())
        self.addCleanup(finish)
        return stream

    def next_inactive(self, stream, timeout=5):
        received, line = stream.get(timeout=timeout)
        self.assertIn('[SENTINEL_INACTIVE]', line)
        self.assertIsNone(self.process.poll(), 'repeat reminder must preserve observer')
        self.assertNotIn('[SENTINEL_COMPLETED]', line)
        self.assertNotIn('[SENTINEL_FAILED]', line)
        return received

    def test_repeat_inactivity_keeps_observer_and_delayed_completion(self):
        stream = self.repeat_stream()
        times = [self.next_inactive(stream) for _ in range(3)]
        gaps = [b-a for a,b in zip(times, times[1:])]
        print('REPEAT_IDLE_GAPS', gaps)
        for gap in gaps:
            self.assertGreaterEqual(gap, 0.75)
            self.assertLess(gap, 1.8)
        self.status('RUNNING', 'TASK-ONE')
        self.done()
        time.sleep(0.15)
        self.audit()
        self.assertIsNone(self.process.poll(), 'same-task owner must delay completion')
        self.status('IDLE')
        _, line = stream.get(timeout=5)
        self.assertIn('[SENTINEL_COMPLETED]', line)
        self.process.wait(timeout=5)
        self.assertEqual(self.process.returncode, 0)

    def test_repeat_inactivity_resets_for_main_and_other_active_owner(self):
        import queue
        stream = self.repeat_stream(interval='2')
        self.next_inactive(stream)
        self.status('RUNNING', 'TASK-ONE')
        with self.assertRaises(queue.Empty):
            stream.get(timeout=2.3)
        other = self.ns / 'worker.helper.status'
        other.write_text(json.dumps({'state': 'VERIFYING', 'current_task': 'TASK-ONE', 'pid': os.getpid()}))
        self.status('IDLE')
        with self.assertRaises(queue.Empty):
            stream.get(timeout=2.3)
        other.write_text(json.dumps({'state': 'IDLE', 'current_task': '', 'pid': os.getpid()}))
        idle_start = time.monotonic()
        reminder = self.next_inactive(stream)
        print('RESET_IDLE_DELAY', reminder-idle_start)
        self.assertGreaterEqual(reminder-idle_start, 0.8)
        self.done()
        self.audit()
        _, line = stream.get(timeout=5)
        self.assertIn('[SENTINEL_COMPLETED]', line)
        self.process.wait(timeout=5)
        self.assertEqual(self.process.returncode, 0)

    def test_repeat_mode_rejects_invalid_setting_before_start(self):
        proc = self.start(limit='0', expect_sentinel=False,
                          env_overrides={'WORKER_INACTIVE_REPEAT': '2'})
        try:
            out, err = proc.communicate(timeout=2)
            self.assertEqual(proc.returncode, 1, out+err)
            self.assertIn('Invalid WORKER_INACTIVE_REPEAT', err)
            self.assertNotIn('[SENTINEL]', out)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.communicate(timeout=3)

    def test_repeat_relay_contract_is_persistent_in_planner_and_explore(self):
        planner = (ROOT / '.agents/skills/nightshift/planner/SKILL.md').read_text()
        explore = (ROOT / '.agents/skills/nightshift/explore/SKILL.md').read_text()
        for required in ('WORKER_INACTIVE_REPEAT=1 bash nightshift/watch_worker.sh',
                         '[SENTINEL_INACTIVE]', 'send_message', 'same watcher process',
                         'user-facing inactivity reminder', 'observer fault'):
            self.assertIn(required, planner)
        self.assertIn('WORKER_INACTIVE_REPEAT=1 bash nightshift/watch_worker.sh', explore)
        self.assertIn('repeated inactivity relay', explore)
