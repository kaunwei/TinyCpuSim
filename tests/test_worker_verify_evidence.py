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




class MechanicalVerifyEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mechanical-evidence-public-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.work = self.root / "repo"
        self.work.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Mechanical Evidence Fixture")
        self.git("config", "user.email", "mechanical@example.invalid")
        (self.work / "baseline.txt").write_text("baseline\n")
        (self.work / ".gitignore").write_text(".reports/\n")
        self.git("add", ".")
        self.git("commit", "-qm", "baseline")
        self.base = self.git("rev-parse", "HEAD").stdout.strip()
        self.stdout_marker = "REAL_MECHANICAL_STDOUT_" + uuid.uuid4().hex
        self.stderr_marker = "REAL_MECHANICAL_STDERR_" + uuid.uuid4().hex

    def git(self, *args):
        result = subprocess.run(["git", "-C", str(self.work), *args],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def emitter(self, code=7, extra="", large=False, sleep=False):
        script = self.root / "verify.py"
        script.write_text(
            "import sys,time\n"
            + ("print('DROP_HEAD:' + '前🙂é' * 4000,flush=True)\n" if large else "")
            + f"print({self.stdout_marker!r},flush=True)\n"
            + f"print({self.stderr_marker!r},file=sys.stderr,flush=True)\n"
            + f"print({extra!r},flush=True)\n"
            + ("time.sleep(3)\n" if sleep else "")
            + f"sys.exit({code})\n", encoding="utf-8")
        return shlex.join([sys.executable, str(script)])

    def audit(self, command, kind="code", env=None, timeout="2"):
        target = "baseline.txt"
        title = "TASK-EVIDENCE-69"
        if kind == "research":
            title += " [RESEARCH]"
            report = self.work / ".reports/current/SUMMARY.md"
            report.parent.mkdir(parents=True, exist_ok=True)
            report.write_text("# Actual findings\nSource: baseline.txt#L1\n")
            target = ".reports/current/SUMMARY.md"
        elif kind == "diagnostic":
            title += " [DIAGNOSTIC]"
        result = subprocess.run([
            sys.executable, str(RUNTIME), "mechanical-audit", "--work-dir", str(self.work),
            "--base", self.base, "--task-id", "TASK-EVIDENCE-69", "--title", title,
            "--target", target, "--verify", command, "--verify-timeout", timeout,
        ], capture_output=True, text=True, encoding="utf-8", timeout=10, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def assert_record(self, record, command, code, stdout=None, stderr=None):
        self.assertEqual(record["command"], command)
        self.assertEqual(record["exit_code"], code)
        self.assertEqual(record["stream_mode"], "stdout_stderr_combined")
        self.assertEqual(record["verify_output_limit_bytes"], 16384)
        self.assertLessEqual(len(record["verify_output"].encode("utf-8")), 16384)
        self.assertIn(stdout or self.stdout_marker, record["verify_output"])
        self.assertIn(stderr or self.stderr_marker, record["verify_output"])

    def test_code_failed_verify_retains_actual_output_and_status(self):
        command = self.emitter()
        result = self.audit(command)
        self.assertEqual((result["status"], result["verify_exit"]), (6, 7))
        self.assertEqual(len(result["verify_evidence"]), 1)
        record = result["verify_evidence"][0]
        self.assertEqual(record["stage"], "verify")
        self.assert_record(record, command, 7)
        self.assertFalse(record["verify_output_truncated"])

    def test_research_custom_verify_retains_output_and_failure_status(self):
        command = self.emitter()
        result = self.audit(command, kind="research")
        self.assertEqual((result["status"], result["verify_exit"]), (6, 7))
        self.assert_record(result["verify_evidence"][0], command, 7)

    def test_diagnostic_expected_red_stays_accepted_with_evidence(self):
        command = self.emitter(extra="AssertionError: actual expected reproduction")
        result = self.audit(command, kind="diagnostic")
        self.assertEqual((result["status"], result["verify_exit"]), (0, 7))
        self.assert_record(result["verify_evidence"][0], command, 7)

    def test_diagnostic_compile_error_stays_false_red(self):
        command = self.emitter(extra="syntax error: actual fixture")
        result = self.audit(command, kind="diagnostic")
        self.assertEqual((result["status"], result["verify_exit"]), (4, 7))
        self.assert_record(result["verify_evidence"][0], command, 7)

    def test_code_zero_tests_keeps_guard_with_executed_output(self):
        command = self.emitter(code=0, extra="Ran 0 tests")
        result = self.audit(command)
        self.assertEqual((result["status"], result["verify_exit"]), (9, 0))
        self.assert_record(result["verify_evidence"][0], command, 0)

    def test_each_category_caps_actual_unicode_tail(self):
        for kind, status in (("code", 6), ("research", 6), ("diagnostic", 0)):
            with self.subTest(kind=kind):
                command = self.emitter(large=True)
                result = self.audit(command, kind=kind)
                self.assertEqual((result["status"], result["verify_exit"]), (status, 7))
                record = result["verify_evidence"][0]
                self.assert_record(record, command, 7)
                output = record["verify_output"]
                self.assertGreaterEqual(len(output.encode("utf-8")), 16381)
                self.assertTrue(record["verify_output_truncated"])
                self.assertNotIn("DROP_HEAD:", output)
                self.assertNotIn("\ufffd", output)

    def test_regression_keeps_distinct_main_and_regression_records(self):
        import os
        (self.work / "Package.swift").write_text("// fixture only\n")
        self.git("add", "Package.swift")
        self.git("commit", "-qm", "declare regression fixture")
        self.base = self.git("rev-parse", "HEAD").stdout.strip()
        tools = self.root / "bin"
        tools.mkdir()
        regression_out = "REAL_REGRESSION_STDOUT_" + uuid.uuid4().hex
        regression_err = "REAL_REGRESSION_STDERR_" + uuid.uuid4().hex
        swift = tools / "swift"
        swift.write_text("#!/usr/bin/python3\nimport sys\n"
                         + "assert sys.argv[1:]==['test']\n"
                         + f"print({regression_out!r},flush=True)\n"
                         + f"print({regression_err!r},file=sys.stderr,flush=True)\n"
                         + "sys.exit(7)\n")
        swift.chmod(0o755)
        command = self.emitter(code=0, extra="Ran 1 test")
        result = self.audit(command, env={**os.environ, "PATH": str(tools) + os.pathsep + os.environ["PATH"]})
        self.assertEqual((result["status"], result["verify_exit"]), (10, 0))
        records = result["verify_evidence"]
        self.assertEqual([r["stage"] for r in records], ["verify", "regression"])
        self.assert_record(records[0], command, 0)
        self.assert_record(records[1], "swift test", 7, regression_out, regression_err)
        self.assertNotIn(regression_out, records[0]["verify_output"])
        self.assertNotIn(self.stdout_marker, records[1]["verify_output"])

    def test_guard_before_execution_has_no_invented_evidence(self):
        target = self.work / "unannounced.txt"
        target.write_text("undeclared\n")
        self.git("add", "unannounced.txt")
        self.git("commit", "-qm", "unannounced delta")
        marker = self.root / "must-not-run"
        command = shlex.join([sys.executable, "-c", f"from pathlib import Path;Path({str(marker)!r}).touch()"])
        result = self.audit(command)
        self.assertEqual((result["status"], result["verify_exit"]), (2, 0))
        self.assertEqual(result["verify_evidence"], [])
        self.assertFalse(marker.exists())

    def test_timeout_retains_actual_output_and_existing_status(self):
        command = self.emitter(sleep=True)
        result = self.audit(command, timeout="0.1")
        self.assertEqual((result["status"], result["verify_exit"]), (6, 124))
        self.assert_record(result["verify_evidence"][0], command, 124)


if __name__ == "__main__":
    unittest.main()
