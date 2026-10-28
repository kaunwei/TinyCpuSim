"""Finite public regression for VERIFY timeout with inherited pipe holders."""
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import unittest

RUNTIME = Path(__file__).resolve().parents[1] / 'nightshift' / 'worker_runtime.py'


def is_live(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().split()[2] != 'Z'
    except FileNotFoundError:
        return False


@unittest.skipUnless(os.name == 'posix' and Path('/proc').is_dir(), 'Linux process-group fixture')
class WorkerVerifyCleanupTest(unittest.TestCase):
    def exercise(self, detached, public):
        with tempfile.TemporaryDirectory(prefix='verify-cleanup-test-') as temporary:
            fixture_root = Path(temporary)
            root = fixture_root / "repo"
            root.mkdir()
            for arguments in [('init', '-q'), ('config', 'user.name', 'Verify Test'),
                              ('config', 'user.email', 'verify@example.invalid')]:
                subprocess.run(['git', '-C', str(root), *arguments], check=True,
                               capture_output=True, timeout=3)
            (root / 'baseline').write_text('baseline\n')
            for arguments in [('add', 'baseline'), ('commit', '-qm', 'baseline')]:
                subprocess.run(['git', '-C', str(root), *arguments], check=True,
                               capture_output=True, timeout=3)
            ready = fixture_root / 'owned-pids.json'
            emitter = fixture_root / 'emitter.py'
            emitter.write_text(
                'import subprocess,sys,os,time,json\nfrom pathlib import Path\n'
                + f'p=subprocess.Popen([sys.executable,"-c","import time; time.sleep(30)"],start_new_session={detached!r})\n'
                + f'Path({str(ready)!r}).write_text(json.dumps({{"leader":os.getpid(),"leader_pgid":os.getpgrp(),"descendant":p.pid,"descendant_pgid":os.getpgid(p.pid)}}))\n'
                + 'print("PIPE_READY",flush=True)\nprint("PIPE_STDERR",file=sys.stderr,flush=True)\ntime.sleep(30)\n')
            command = shlex.join([sys.executable, str(emitter)])
            if public:
                arguments = [sys.executable, str(RUNTIME), 'mechanical-audit',
                             '--work-dir', str(root), '--base', 'HEAD', '--target', 'baseline',
                              '--task-id', 'VERIFY-CLEANUP', '--title', 'code', '--verify', command,
                              '--verify-timeout', '0.2']
            else:
                caller = (
                    'import importlib.util,json,sys\n'
                    'spec=importlib.util.spec_from_file_location("runtime_probe",sys.argv[1])\n'
                    'runtime=importlib.util.module_from_spec(spec);spec.loader.exec_module(runtime)\n'
                    'code,output=runtime.run_verify(sys.argv[2],sys.argv[3],"3001",0.2)\n'
                    'print(json.dumps({"code":code,"output":output}))\n')
                arguments = [sys.executable, '-c', caller, str(RUNTIME), command, str(root)]
            process = subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       text=True, start_new_session=True)
            started = time.monotonic()
            pids = {}
            try:
                readiness_deadline = started + 2
                while not ready.exists() and time.monotonic() < readiness_deadline:
                    time.sleep(0.01)
                self.assertTrue(ready.exists(), 'fixture failed to publish owned PIDs')
                pids = json.loads(ready.read_text())
                try:
                    stdout, stderr = process.communicate(timeout=max(0.01, started + 5 - time.monotonic()))
                except subprocess.TimeoutExpired:
                    self.fail('VERIFY waited for escaped pipe EOF beyond finite cleanup deadline')
                self.assertLess(time.monotonic() - started, 5)
                self.assertEqual(process.returncode, 0, stderr)
                result = json.loads(stdout)
                if public:
                    self.assertEqual((result['status'], result['verify_exit']), (6, 124))
                else:
                    self.assertEqual(result['code'], 124)
                    self.assertEqual(result['output'].count('PIPE_READY\n'), 1)
                    self.assertEqual(result['output'].count('PIPE_STDERR\n'), 1)
                self.assertFalse(is_live(pids['leader']), 'owned direct leader was not reaped')
                if not detached:
                    self.assertFalse(is_live(pids['descendant']), 'same-group child survived timeout')
            finally:
                # Include escaped group ONLY because this fixture explicitly created and recorded it.
                if not pids and ready.exists():
                    pids = json.loads(ready.read_text())
                for pgid in {process.pid, pids.get('leader_pgid'), pids.get('descendant_pgid')} - {None}:
                    if pgid == os.getpgrp():
                        continue
                    try:
                        os.killpg(pgid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                process.communicate(timeout=2)
                cleanup_deadline = time.monotonic() + 1
                owned = [process.pid, *[pids[key] for key in ('leader', 'descendant') if key in pids]]
                while any(is_live(pid) for pid in owned) and time.monotonic() < cleanup_deadline:
                    time.sleep(0.01)
                self.assertFalse(any(is_live(pid) for pid in owned), 'test-owned process survived cleanup')

    def test_public_timeout_with_detached_pipe_holder_is_finite(self):
        self.exercise(detached=True, public=True)

    def test_public_same_group_timeout_still_cleans_descendant(self):
        self.exercise(detached=False, public=True)

    def test_run_verify_preserves_cumulative_combined_output(self):
        self.exercise(detached=True, public=False)


if __name__ == '__main__':
    unittest.main()
