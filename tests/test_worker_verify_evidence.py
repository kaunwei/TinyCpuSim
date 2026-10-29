"""Actual finalizer VERIFY evidence through the public fake-adapter protocol."""
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
import uuid

RUNTIME = Path(__file__).resolve().parents[1] / "nightshift/worker_runtime.py"


class WorkerVerifyEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="verify-evidence-public-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.work = self.root / "repo"
        self.work.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Evidence Fixture")
        self.git("config", "user.email", "evidence@example.invalid")
        (self.work / "baseline.txt").write_text("before\n")
        self.git("add", "baseline.txt")
        self.git("commit", "-qm", "baseline")
        self.base = self.git("rev-parse", "HEAD").stdout.strip()
        self.stdout_marker = "ACTUAL_STDOUT_" + uuid.uuid4().hex
        self.stderr_marker = "ACTUAL_STDERR_" + uuid.uuid4().hex
        self.adapter = self.root / "fake_adapter.py"
        self.adapter.write_text(
            "import json,pathlib,sys\nq=json.load(sys.stdin);op=sys.argv[1]\n"
            "if op=='get_config': r={'protocol_version':1,'initial_score':5,'limits':{'poll_interval_seconds':0.01}}\n"
            "elif op=='start':\n"
            " pathlib.Path(q['working_directory'],'baseline.txt').write_text('after\\n')\n"
            " pathlib.Path(q['result_path']).write_text(json.dumps({'outcome':'complete','changed':True,'commit_message':'fix(evidence): verified fixture','verification_evidence':'ADAPTER_CLAIM_ONLY'}))\n"
            " p=pathlib.Path(q['output_dir'])/'status.json';p.write_text(json.dumps({'status':'complete','result_location':q['result_path']}));r={'execution_id':'fixture','status_location':str(p)}\n"
            "else: r=json.loads(pathlib.Path(q['status_location']).read_text())\n"
            "print(json.dumps(r))\n")
        self.prompt = self.root / "prompt.txt"
        self.prompt.write_text("fake adapter only; no providers")

    def git(self, *args):
        result = subprocess.run(["git", "-C", str(self.work), *args],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def run_task(self, code, large=False):
        emitter = self.root / "verify.py"
        emitter.write_text(
            "import sys\n"
            + ("sys.stdout.write('DROP_HEAD:' + '前🙂é' * 4000 + '\\n');sys.stdout.flush()\n" if large else "")
            + f"print({self.stdout_marker!r},flush=True)\n"
            + f"print({self.stderr_marker!r},file=sys.stderr,flush=True)\n"
            + f"sys.exit({code})\n", encoding="utf-8")
        result_path = self.root / "result.json"
        output_path = self.root / "output.json"
        process = subprocess.run([
            sys.executable, str(RUNTIME), "task-cli", "--adapter", "fake",
            "--executable", sys.executable, "--extra-arg", str(self.adapter),
            "--work-dir", str(self.work), "--prompt-file", str(self.prompt),
            "--output-file", str(output_path), "--result-file", str(result_path),
            "--task-id", "TASK-EVIDENCE-68", "--title", "TASK-EVIDENCE-68",
            "--target", "baseline.txt", "--base", self.base,
            "--verify", shlex.join([sys.executable, str(emitter)]), "--timeout", "2",
        ], capture_output=True, text=True, timeout=15, encoding="utf-8")
        self.assertTrue(result_path.exists(), process.stderr + process.stdout)
        result = json.loads(result_path.read_text(encoding="utf-8"))
        wrapper = json.loads(output_path.read_text(encoding="utf-8"))
        self.assertEqual(wrapper["result"], result)
        return process, result

    def assert_failure(self, process, result):
        self.assertEqual(process.returncode, 1, process.stderr)
        self.assertEqual(result["outcome"], "complete", "adapter outcome semantics must remain unchanged")
        error = result["runtime_error"]
        self.assertEqual((error["phase"], error["code"]), ("verification", 7))
        self.assertIn("refusing script-owned commit", error["message"])
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), self.base)
        self.assertEqual((self.work / "baseline.txt").read_text(), "after\n")
        self.assertEqual(self.git("diff", "--cached", "--name-only").stdout, "")
        self.assertEqual(error["verify_output_limit_bytes"], 16384)
        self.assertEqual(error["stream_mode"], "stdout_stderr_combined")
        for marker in (self.stdout_marker, self.stderr_marker):
            self.assertIn(marker, process.stdout)
            self.assertIn(marker, error["verify_output"])
        self.assertNotIn("ADAPTER_CLAIM_ONLY", error["verify_output"])
        return error

    def test_failed_verify_retains_actual_stdout_stderr_without_commit(self):
        process, result = self.run_task(7)
        error = self.assert_failure(process, result)
        self.assertFalse(error["verify_output_truncated"])
        self.assertEqual(error["verify_output"], self.stdout_marker + "\n" + self.stderr_marker + "\n")

    def test_large_unicode_failure_retains_valid_utf8_bounded_tail(self):
        process, result = self.run_task(7, large=True)
        error = self.assert_failure(process, result)
        encoded = error["verify_output"].encode("utf-8")
        self.assertLessEqual(len(encoded), 16384)
        self.assertGreaterEqual(len(encoded), 16381)
        self.assertEqual(encoded.decode("utf-8"), error["verify_output"])
        self.assertNotIn("DROP_HEAD:", error["verify_output"])
        self.assertNotIn("\ufffd", error["verify_output"])
        self.assertTrue(error["verify_output"].endswith(self.stderr_marker + "\n"))
        self.assertTrue(error["verify_output_truncated"])

    def test_successful_verify_preserves_real_commit_and_explicit_message(self):
        process, result = self.run_task(0)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertNotIn("runtime_error", result)
        self.assertNotEqual(self.git("rev-parse", "HEAD").stdout.strip(), self.base)
        self.assertEqual(self.git("log", "-1", "--format=%s").stdout.strip(),
                         "fix(evidence): verified fixture")
        self.assertEqual(self.git("status", "--porcelain").stdout, "")


if __name__ == "__main__":
    unittest.main()
