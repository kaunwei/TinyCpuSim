"""Runtime-owned finalized results survive registered attempt temporary cleanup."""
import json
from pathlib import Path
import shlex
import stat
import subprocess
import sys
import tempfile
import unittest
import uuid

RUNTIME = Path(__file__).resolve().parents[1] / "nightshift/worker_runtime.py"


class WorkerTerminalArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="terminal-artifact-public-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.work = self.root / "repo"
        self.work.mkdir()
        self.artifacts = self.root / "persistent-attempt-adapters"
        self.git("init", "-q")
        self.git("config", "user.name", "Terminal Fixture")
        self.git("config", "user.email", "terminal@example.invalid")
        (self.work / "baseline.txt").write_text("before\n")
        self.git("add", ".")
        self.git("commit", "-qm", "baseline")
        self.head = self.git("rev-parse", "HEAD").stdout.strip()

    def git(self, *args):
        result = subprocess.run(["git", "-C", str(self.work), *args],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def run_case(self, mode, configured=True, obstruct=False):
        attempt_id = "attempt-" + uuid.uuid4().hex
        adapter = self.root / "fake_adapter.py"
        adapter.write_text(
            "import json,pathlib,sys\nq=json.load(sys.stdin);op=sys.argv[1]\n"
            + f"mode={mode!r};obstruct={obstruct!r}\n"
            + "if op=='get_config': r={'protocol_version':1,'initial_score':5,'limits':{'poll_interval_seconds':0.01}}\n"
            + "elif op=='start':\n"
            + " p=pathlib.Path(q['output_dir']);p.mkdir(parents=True,exist_ok=True)\n"
            + " (p/'adapter.log').write_text('FIXTURE_RAW_LOG\\n')\n"
            + " if obstruct:(p/'.runtime-terminal').write_text('adapter fixture blocks runtime directory')\n"
            + " if mode in ('success','verify_failure'):\n"
            + "  pathlib.Path(q['working_directory'],'baseline.txt').write_text('after\\n')\n"
            + "  payload={'outcome':'complete','changed':True,'commit_message':'fix(terminal): fixture'}\n"
            + " elif mode=='blocked':payload={'outcome':'blocked','diagnostic':'fixture blocked'}\n"
            + " elif mode=='budget':payload={'outcome':'step_budget_exceeded','actions_used':3,'error_code':'step_budget_exceeded'}\n"
            + " else:payload=None\n"
            + " status={'status':{'success':'complete','verify_failure':'complete','blocked':'failed','no_response':'no_response','budget':'step_budget_exceeded'}[mode],'fixture_private':'STATUS_SECRET_CANARY'}\n"
            + " if payload is not None:\n"
            + "  pathlib.Path(q['result_path']).write_text(json.dumps(payload));status['result_location']=q['result_path']\n"
            + " (p/'status.json').write_text(json.dumps(status));(p/'status-before.txt').write_text(json.dumps(status))\n"
            + " r={'execution_id':'fixture-execution','status_location':str(p/'status.json')}\n"
            + "else:r=json.loads(pathlib.Path(q['status_location']).read_text())\n"
            + "print(json.dumps(r))\n")
        driver = self.root / "driver.py"
        driver.write_text(
            "import argparse,importlib.util,json,sys\n"
            "spec=importlib.util.spec_from_file_location('runtime_under_test',sys.argv[1]);r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)\n"
            "args=argparse.Namespace(**json.load(open(sys.argv[2])))\n"
            "sys.exit(r.execute_adapter_task(args))\n")
        marker_out = "REAL_TERMINAL_STDOUT_" + uuid.uuid4().hex
        marker_err = "REAL_TERMINAL_STDERR_" + uuid.uuid4().hex
        emitter = self.root / "verify.py"
        emitter.write_text("import sys\n" + f"print({marker_out!r},flush=True)\n"
                           + f"print({marker_err!r},file=sys.stderr,flush=True)\n"
                           + f"sys.exit({7 if mode=='verify_failure' else 0})\n")
        with tempfile.TemporaryDirectory(prefix="ephemeral-task-", dir=self.root) as directory:
            temporary = Path(directory)
            prompt = temporary / "prompt.txt"
            prompt.write_text("PROMPT_SECRET_CANARY")
            output = temporary / "cli-output.log"
            result = temporary / "last-message.txt"
            args = dict(adapter="fake", executable=sys.executable, extra_args=[str(adapter)],
                        work_dir=str(self.work), prompt_file=str(prompt), output_file=str(output),
                        result_file=str(result), attempt_id=attempt_id, timeout=2,
                        worker_port="3001", step_budget=10, target="baseline.txt", task_id="TASK-TERMINAL-13",
                        title="TASK-TERMINAL-13", level="3", base=self.head,
                        agent_id="fixture-registration", agent_overrides={"private":"REGISTRY_SECRET_CANARY"},
                        agent_env={"TOKEN":"ENV_SECRET_CANARY"},
                        verify=shlex.join([sys.executable, str(emitter)]) if mode in ('success','verify_failure') else "")
            if configured:
                args["adapter_artifact_dir"] = str(self.artifacts)
            config = self.root / "driver-args.json"
            config.write_text(json.dumps(args))
            process = subprocess.run([sys.executable, str(driver), str(RUNTIME), str(config)],
                                     capture_output=True, text=True, encoding="utf-8", timeout=10)
            self.assertTrue(output.exists(), process.stderr)
            wrapper = json.loads(output.read_text())
            final_result = json.loads(result.read_text())
            self.assertEqual(wrapper["result"], final_result)
            if not configured:
                self.assertNotIn("terminal_artifacts", wrapper)
                self.assertNotIn("terminal_artifact_error", wrapper)
                self.assertEqual(list(temporary.rglob(".runtime-terminal")), [])
        self.assertFalse(temporary.exists(), "temporary result/prompt/output must really be removed")
        return process, wrapper, final_result, attempt_id, marker_out, marker_err

    def assert_durable(self, wrapper, result, attempt, status, code):
        pointers = wrapper["terminal_artifacts"]
        manifest_path = Path(pointers["manifest"])
        result_path = Path(pointers["result"])
        self.assertTrue(manifest_path.is_file())
        self.assertEqual(json.loads(result_path.read_text()), result)
        manifest_text = manifest_path.read_text()
        manifest = json.loads(manifest_text)
        self.assertEqual(set(manifest), {"schema_version", "task_id", "attempt_id", "adapter",
                                       "registration_id", "adapter_terminal_status", "runtime_exit_code", "terminal_result"})
        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["task_id"], "TASK-TERMINAL-13")
        self.assertEqual(manifest["attempt_id"], attempt)
        self.assertEqual(manifest["adapter"], "fake")
        self.assertEqual(manifest["registration_id"], "fixture-registration")
        self.assertEqual((manifest["adapter_terminal_status"], manifest["runtime_exit_code"]), (status, code))
        self.assertEqual((manifest_path.parent / manifest["terminal_result"]).resolve(), result_path.resolve())
        self.assertEqual(stat.S_IMODE(manifest_path.parent.stat().st_mode), 0o700)
        for path in (manifest_path, result_path):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        for canary in ("PROMPT_SECRET_CANARY", "REGISTRY_SECRET_CANARY", "ENV_SECRET_CANARY", "STATUS_SECRET_CANARY"):
            self.assertNotIn(canary, manifest_text)
        attempt_dir = self.artifacts / attempt
        self.assertEqual((attempt_dir / "status.json").read_bytes(), (attempt_dir / "status-before.txt").read_bytes())
        self.assertEqual((attempt_dir / "adapter.log").read_text(), "FIXTURE_RAW_LOG\n")

    def test_success_survives_temporary_cleanup_and_keeps_commit(self):
        process, wrapper, result, attempt, _, _ = self.run_case("success")
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(result["outcome"], "complete")
        self.assertNotIn("runtime_error", result)
        self.assert_durable(wrapper, result, attempt, "complete", 0)
        self.assertNotEqual(self.git("rev-parse", "HEAD").stdout.strip(), self.head)
        self.assertEqual(self.git("log", "-1", "--format=%s").stdout.strip(), "fix(terminal): fixture")

    def test_blocked_noresponse_budget_keep_real_terminal_status(self):
        for mode, status, code, outcome in (("blocked", "failed", 1, "blocked"),
                                            ("no_response", "no_response", 124, None),
                                            ("budget", "step_budget_exceeded", 125, "step_budget_exceeded")):
            with self.subTest(mode=mode):
                process, wrapper, result, attempt, _, _ = self.run_case(mode)
                self.assertEqual(process.returncode, code, process.stderr)
                self.assertEqual(wrapper["status"], status)
                self.assertEqual(result.get("outcome"), outcome)
                self.assert_durable(wrapper, result, attempt, status, code)
                self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), self.head)

    def test_failed_host_verify_retains_final_runtime_metadata_after_cleanup(self):
        process, wrapper, result, attempt, stdout, stderr = self.run_case("verify_failure")
        self.assertEqual(process.returncode, 1, process.stderr)
        self.assert_durable(wrapper, result, attempt, "complete", 1)
        error = result["runtime_error"]
        self.assertEqual((error["phase"], error["code"]), ("verification", 7))
        self.assertIn(stdout, error["verify_output"])
        self.assertIn(stderr, error["verify_output"])
        self.assertEqual(error["verify_output_limit_bytes"], 16384)
        self.assertFalse(error["verify_output_truncated"])
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), self.head)

    def test_unconfigured_artifact_root_preserves_legacy_copy_behavior(self):
        process, _, result, _, _, _ = self.run_case("success", configured=False)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(result["outcome"], "complete")
        self.assertFalse(self.artifacts.exists())

    def test_persistence_failure_is_visible_without_reclassifying_success(self):
        process, wrapper, result, attempt, _, _ = self.run_case("success", obstruct=True)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(result["outcome"], "complete")
        self.assertNotIn("runtime_error", result)
        self.assertNotIn("terminal_artifacts", wrapper)
        self.assertEqual(wrapper["terminal_artifact_error"]["phase"], "terminal_persistence")
        self.assertIn("terminal evidence persistence failed", process.stderr)
        self.assertNotEqual(self.git("rev-parse", "HEAD").stdout.strip(), self.head)
        attempt_dir = self.artifacts / attempt
        self.assertEqual((attempt_dir / "status.json").read_bytes(), (attempt_dir / "status-before.txt").read_bytes())


if __name__ == "__main__":
    unittest.main()
