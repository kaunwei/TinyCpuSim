"""Offline real AgY protocol tests: action observations, never unique execution claims."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "nightshift/worker_runtime.py"
ADAPTER = ROOT / "nightshift/adapters/agy.py"


class AgyActionSourceTests(unittest.TestCase):
    def native(self):
        return json.dumps({"type": "item.completed", "item": {
            "type": "command_execution", "id": "command-1", "command": "echo [EXEC] literal"}})

    def exercise(self, lines, expected_native, expected_markers, limit=12, budget=False):
        with tempfile.TemporaryDirectory(prefix="action-source-public-") as directory:
            root = Path(directory)
            work, durable = root / "repo", root / "persistent"
            work.mkdir()
            for args in (("init", "-q"), ("config", "user.name", "Action Fixture"),
                         ("config", "user.email", "action@example.invalid")):
                subprocess.run(["git", "-C", str(work), *args], capture_output=True, check=True, timeout=5)
            (work / "baseline.txt").write_text("baseline\n")
            for args in (("add", "."), ("commit", "-qm", "baseline")):
                subprocess.run(["git", "-C", str(work), *args], capture_output=True, check=True, timeout=5)
            head = subprocess.check_output(["git", "-C", str(work), "rev-parse", "HEAD"], text=True).strip()
            cli = root / "fake_agy.py"
            closeout = json.dumps({"outcome": "no_change", "changed": False,
                                   "no_change_reason": "offline fixture no edits required",
                                   "actions_used": 999,
                                   "action_sources": {"native_completed_command_events": 999}})
            cli.write_text("#!/usr/bin/python3\nimport sys,time\n"
                           "if '--help' in sys.argv: print('offline fixture help');sys.exit(0)\n"
                           + f"lines={lines + [closeout]!r}\n"
                           + "for line in lines: print(line,flush=True)\n"
                           + ("time.sleep(2)\n" if budget else ""))
            cli.chmod(0o755)
            driver = root / "driver.py"
            driver.write_text("import argparse,importlib.util,json,sys\n"
                              "spec=importlib.util.spec_from_file_location('runtime',sys.argv[1]);r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)\n"
                              "sys.exit(r.execute_adapter_task(argparse.Namespace(**json.load(open(sys.argv[2])))))\n")
            attempt = "attempt-" + uuid.uuid4().hex
            with tempfile.TemporaryDirectory(prefix="ephemeral-task-", dir=root) as temporary:
                ephemeral = Path(temporary)
                prompt, output, result_path = ephemeral / "prompt", ephemeral / "output", ephemeral / "result"
                prompt.write_text("PROMPT_SECRET_CANARY")
                args = dict(adapter="agy", executable=str(cli), adapter_command=sys.executable,
                            adapter_args=[str(ADAPTER)], work_dir=str(work), prompt_file=str(prompt),
                            output_file=str(output), result_file=str(result_path), adapter_artifact_dir=str(durable),
                            attempt_id=attempt, timeout=5, worker_port="3001", step_budget=limit,
                            task_id="TASK-ACTION-13", title="TASK-ACTION-13", level="3", target="baseline.txt", base=head,
                            agent_id="offline-agy-fixture", agent_overrides={"command":str(cli)},
                            agent_env={"TOKEN":"ENV_SECRET_CANARY"}, verify="")
                request = root / "args.json"
                request.write_text(json.dumps(args))
                completed = subprocess.run([sys.executable,str(driver),str(RUNTIME),str(request)],
                                           capture_output=True,text=True,timeout=10)
                self.assertTrue(output.exists(),completed.stderr)
                wrapper=json.loads(output.read_text())
                result=json.loads(result_path.read_text())
            self.assertFalse(ephemeral.exists())
            expected_status = "step_budget_exceeded" if budget else "complete"
            self.assertEqual(completed.returncode,125 if budget else 0,completed.stderr)
            self.assertEqual(wrapper["status"],expected_status)
            self.assertEqual(result["outcome"],"step_budget_exceeded" if budget else "no_change")
            self.assertEqual(result["actions_used"],expected_native+expected_markers)
            observed = result["action_sources"]
            self.assertEqual(observed,{
                "native_completed_command_events":expected_native,
                "printed_exec_markers":expected_markers,
                "counting_policy":"legacy_observations",
                "cross_source_identity":"unknown",
            })
            self.assertEqual(wrapper["result"],result)
            durable_result=Path(wrapper["terminal_artifacts"]["result"])
            self.assertEqual(json.loads(durable_result.read_text()),result)
            manifest=Path(wrapper["terminal_artifacts"]["manifest"])
            self.assertNotIn("PROMPT_SECRET_CANARY",manifest.read_text())
            self.assertNotIn("ENV_SECRET_CANARY",manifest.read_text())
            self.assertNotIn("echo",json.dumps(observed))
            status=json.loads((durable/attempt/"status.json").read_text())
            self.assertEqual(set(status),{"status","result_location","exit_code"})
            self.assertEqual(status["status"],expected_status)
            self.assertEqual(status["exit_code"],result["cli_exit_code"])
            self.assertFalse(Path(status["result_location"]).exists(),"adapter's temporary path intentionally unchanged")
            raw=(durable/attempt/"agy.log").read_text()
            for line in lines:self.assertIn(line,raw)
            self.assertEqual(subprocess.check_output(["git","-C",str(work),"rev-parse","HEAD"],text=True).strip(),head)

    def test_native_only_observation_count_and_durable_result(self):
        self.exercise([self.native()],1,0)

    def test_printed_marker_only_is_not_native_completion(self):
        self.exercise(["[EXEC] narrated action","[EXEC] another narrated action"],0,2)

    def test_mixed_same_text_is_not_recounted_or_semantically_deduplicated(self):
        native=self.native()
        message=json.dumps({"type":"item.completed","item":{"type":"message","text":"[EXEC] narrative"}})
        self.exercise([native,"[EXEC] command-1 completed",message],1,1,limit=2)

    def test_repeated_native_events_keep_legacy_observation_count(self):
        self.exercise([self.native(),self.native()],2,0,limit=2)

    def test_mixed_n_plus_one_retains_original_budget_enforcement(self):
        self.exercise([self.native(),"[EXEC] narrated action",self.native()],2,1,limit=2,budget=True)

    def test_noncommand_json_and_embedded_event_strings_count_zero(self):
        text=json.dumps({"type":"item.completed","item":{"type":"message","text":self.native()+" [EXEC]"}})
        self.exercise([text],0,0)


if __name__ == "__main__":
    unittest.main()
