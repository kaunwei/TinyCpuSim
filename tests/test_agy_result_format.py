import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ADAPTER_PATH = Path(__file__).resolve().parent.parent / "nightshift" / "adapters" / "agy.py"


class TestAgyResultFormat(unittest.TestCase):
    def run_adapter_start_and_wait(self, fake_cli_script_content, action_limit=12):
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_path = Path(tmpdir)
            fake_cli = tmp_path / "fake_agy.sh"
            fake_cli.write_text(f"#!/bin/sh\n{fake_cli_script_content}\n")
            fake_cli.chmod(0o755)

            prompt_path = tmp_path / "prompt.txt"
            prompt_path.write_text("Test prompt\n")

            output_dir = tmp_path / "output"
            result_path = tmp_path / "result.json"

            start_req = {
                "protocol_version": 1,
                "configuration": {
                    "executable": str(fake_cli),
                    "action_limit": action_limit,
                    "execution_timeout_seconds": 10,
                },
                "working_directory": str(tmp_path),
                "output_dir": str(output_dir),
                "result_path": str(result_path),
                "prompt_path": str(prompt_path),
            }

            p_start = subprocess.run(
                [sys.executable, str(ADAPTER_PATH), "start"],
                input=json.dumps(start_req),
                capture_output=True,
                text=True,
                check=True,
            )
            start_resp = json.loads(p_start.stdout)
            status_loc = start_resp["status_location"]

            status_req = {
                "protocol_version": 1,
                "status_location": status_loc,
            }
            # Poll status
            for _ in range(50):
                p_status = subprocess.run(
                    [sys.executable, str(ADAPTER_PATH), "status"],
                    input=json.dumps(status_req),
                    capture_output=True,
                    text=True,
                    check=True,
                )
                status_resp = json.loads(p_status.stdout)
                if status_resp.get("status") != "running":
                    break
                import time
                time.sleep(0.1)
            else:
                self.fail("Adapter did not finish in time")

            result_data = None
            if result_path.exists():
                result_data = json.loads(result_path.read_text(encoding="utf-8"))
            return status_resp, result_data

    def test_single_line_json_result(self):
        script = 'echo \'[EXEC] step 1\'\necho \'{"outcome": "complete", "changed": true}\''
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "complete")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "complete")
        self.assertTrue(result_data["changed"])

    def test_multiline_pretty_printed_json_result(self):
        script = """echo '[EXEC] step 1'
echo '[EXEC] step 2'
echo '{'
echo '  "outcome": "complete",'
echo '  "changed": true,'
echo '  "details": {'
echo '    "note": "nested object with { braces } and \\"quotes\\""'
echo '  }'
echo '}'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "complete")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "complete")
        self.assertTrue(result_data["changed"])
        self.assertEqual(result_data["details"]["note"], 'nested object with { braces } and "quotes"')

    def test_multiline_json_with_trailing_whitespace(self):
        script = """echo '[EXEC] step 1'
echo '{'
echo '  "outcome": "complete",'
echo '  "changed": false'
echo '}'
echo ''
echo '   '
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "complete")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "no_change" if result_data.get("outcome") == "no_change" else "complete")

    def test_fenced_json_result_preserved(self):
        script = """echo '```json'
echo '{'
echo '  "outcome": "complete",'
echo '  "changed": true'
echo '}'
echo '```'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "complete")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "complete")

    def test_no_response_or_failure_preserved(self):
        script = """echo 'Some failure happened without valid json'
exit 1
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "no_response")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "no_response")

    def test_budget_exceeded_preserved(self):
        script = """for i in $(seq 1 15); do
    echo "[EXEC] step $i"
done
echo '{"outcome": "complete"}'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script, action_limit=5)
        self.assertEqual(status_resp["status"], "step_budget_exceeded")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "step_budget_exceeded")

    def test_single_json_command_actions_used_one(self):
        cmd_event = json.dumps({"type": "item.completed", "item": {"type": "command_execution"}})
        script = f"""echo '{cmd_event}'
echo '{{"outcome": "complete"}}'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script, action_limit=5)
        self.assertEqual(status_resp["status"], "complete")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "complete")
        self.assertEqual(result_data.get("actions_used"), 1)

    def test_exact_limit_n_succeeds(self):
        cmd_event = json.dumps({"type": "item.completed", "item": {"type": "command_execution"}})
        script = f"""echo '{cmd_event}'
echo '{cmd_event}'
echo '{{"outcome": "complete"}}'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script, action_limit=2)
        self.assertEqual(status_resp["status"], "complete")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "complete")
        self.assertEqual(result_data.get("actions_used"), 2)

    def test_n_plus_one_budget_exceeded(self):
        cmd_event = json.dumps({"type": "item.completed", "item": {"type": "command_execution"}})
        script = f"""echo '{cmd_event}'
echo '{cmd_event}'
echo '{cmd_event}'
echo '{{"outcome": "complete"}}'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script, action_limit=2)
        self.assertEqual(status_resp["status"], "step_budget_exceeded")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "step_budget_exceeded")

    def test_non_command_actions_zero(self):
        msg_event = json.dumps({"type": "item.completed", "item": {"type": "message", "text": "hello"}})
        script = f"""echo '{msg_event}'
echo '{{"outcome": "complete"}}'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script, action_limit=5)
        self.assertEqual(status_resp["status"], "complete")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "complete")
        self.assertEqual(result_data.get("actions_used"), 0)

    def test_terminal_bare_blocked_overrides_earlier_fenced_complete(self):
        script = """echo '[EXEC] step 1'
echo '```json'
echo '{"outcome": "complete", "changed": true}'
echo '```'
echo '[EXEC] step 2'
echo '{"outcome": "blocked"}'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "failed")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "blocked")

    def test_terminal_fenced_complete_overrides_earlier_bare_blocked(self):
        script = """echo '[EXEC] step 1'
echo '{"outcome": "blocked"}'
echo '[EXEC] step 2'
echo '```json'
echo '{"outcome": "complete", "changed": true}'
echo '```'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "complete")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "complete")
        self.assertTrue(result_data.get("changed"))

    def test_terminal_fenced_blocked_overrides_earlier_bare_complete(self):
        script = """echo '[EXEC] step 1'
echo '{"outcome": "complete", "changed": true}'
echo '[EXEC] step 2'
echo '```json'
echo '{"outcome": "blocked"}'
echo '```'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "failed")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "blocked")

    def test_terminal_bare_complete_overrides_earlier_fenced_blocked(self):
        script = """echo '[EXEC] step 1'
echo '```json'
echo '{"outcome": "blocked"}'
echo '```'
echo '[EXEC] step 2'
echo '{"outcome": "complete", "changed": true}'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "complete")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "complete")
        self.assertTrue(result_data.get("changed"))

    def test_terminal_pretty_blocked_overrides_earlier_bare_complete(self):
        script = """echo '[EXEC] step 1'
echo '{"outcome": "complete", "changed": true}'
echo '[EXEC] step 2'
echo '{'
echo '  "outcome": "blocked",'
echo '  "details": {'
echo '    "note": "nested object with { braces } and \\"quotes\\""'
echo '  }'
echo '}'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "failed")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "blocked")
        self.assertEqual(result_data["details"]["note"], 'nested object with { braces } and "quotes"')

    def test_terminal_pretty_blocked_overrides_earlier_fenced_complete(self):
        script = """echo '[EXEC] step 1'
echo '```json'
echo '{'
echo '  "outcome": "complete",'
echo '  "details": {'
echo '    "note": "earlier fenced with { braces } and \\"quotes\\""'
echo '  }'
echo '}'
echo '```'
echo '[EXEC] step 2'
echo '{'
echo '  "outcome": "blocked",'
echo '  "details": {'
echo '    "note": "final pretty with { braces } and \\"quotes\\""'
echo '  }'
echo '}'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "failed")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "blocked")
        self.assertEqual(result_data["details"]["note"], 'final pretty with { braces } and "quotes"')

    def test_terminal_pretty_complete_overrides_earlier_fenced_blocked(self):
        script = """echo '[EXEC] step 1'
echo '```json'
echo '{"outcome": "blocked"}'
echo '```'
echo '[EXEC] step 2'
echo '{'
echo '  "outcome": "complete",'
echo '  "changed": true,'
echo '  "details": {'
echo '    "note": "final pretty complete with { braces } and \\"quotes\\""'
echo '  }'
echo '}'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "complete")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "complete")
        self.assertTrue(result_data.get("changed"))
        self.assertEqual(result_data["details"]["note"], 'final pretty complete with { braces } and "quotes"')

    def test_terminal_bare_complete_overrides_earlier_pretty_blocked(self):
        script = """echo '[EXEC] step 1'
echo '{'
echo '  "outcome": "blocked",'
echo '  "details": {'
echo '    "note": "nested object with { braces } and \\"quotes\\""'
echo '  }'
echo '}'
echo '[EXEC] step 2'
echo '{"outcome": "complete", "changed": true}'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "complete")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "complete")
        self.assertTrue(result_data.get("changed"))

    def test_terminal_fenced_complete_overrides_earlier_pretty_blocked(self):
        script = """echo '[EXEC] step 1'
echo '{'
echo '  "outcome": "blocked",'
echo '  "details": {'
echo '    "note": "nested object with { braces } and \\"quotes\\""'
echo '  }'
echo '}'
echo '[EXEC] step 2'
echo '```json'
echo '{'
echo '  "outcome": "complete",'
echo '  "changed": true,'
echo '  "details": {'
echo '    "note": "final fenced complete with { braces } and \\"quotes\\""'
echo '  }'
echo '}'
echo '```'
"""
        status_resp, result_data = self.run_adapter_start_and_wait(script)
        self.assertEqual(status_resp["status"], "complete")
        self.assertIsNotNone(result_data)
        self.assertEqual(result_data["outcome"], "complete")
        self.assertTrue(result_data.get("changed"))
        self.assertEqual(result_data["details"]["note"], 'final fenced complete with { braces } and "quotes"')



    def test_malformed_terminal_bare_rejects_historical_complete(self):
        status, result = self.run_adapter_start_and_wait("echo '```json'\necho '{\"outcome\":\"complete\"}'\necho '```'\necho '{\"outcome\":\"blocked\"'")
        self.assertEqual((status["status"], result["outcome"], result["error_code"]), ("failed", "failure", "invalid_result"))

    def test_malformed_terminal_fence_rejects_historical_complete(self):
        status, result = self.run_adapter_start_and_wait("echo '{\"outcome\":\"complete\"}'\necho '```json'\necho '{\"outcome\":\"blocked\"'\necho '```'")
        self.assertEqual((status["status"], result["outcome"], result["error_code"]), ("failed", "failure", "invalid_result"))

    def test_unknown_terminal_outcome_rejects_historical_complete(self):
        status, result = self.run_adapter_start_and_wait("echo '```json'\necho '{\"outcome\":\"complete\"}'\necho '```'\necho '{\"outcome\":\"unknown\"}'")
        self.assertEqual((status["status"], result["outcome"], result["error_code"]), ("failed", "failure", "invalid_result"))

    def test_authoritative_message_never_falls_back_to_historical_success(self):
        import shlex
        for content in ('{"outcome":"blocked"', '', '[]', '{"outcome":"unknown"}'):
            with self.subTest(content=content):
                script = "printf %s " + shlex.quote(content) + " > output/agy-message.json\necho '{\"outcome\":\"complete\"}'"
                status, result = self.run_adapter_start_and_wait(script)
                self.assertEqual((status["status"], result["outcome"], result["error_code"]), ("failed", "failure", "invalid_result"))

    def test_valid_authoritative_message_overrides_later_stdout(self):
        status, result = self.run_adapter_start_and_wait("echo '{\"outcome\":\"blocked\"}' > output/agy-message.json\necho '{\"outcome\":\"complete\"}'")
        self.assertEqual((status["status"], result["outcome"]), ("failed", "blocked"))

    def test_malformed_nested_result_cannot_promote_inner_success(self):
        script = "echo '{\"outcome\":\"blocked\",'\necho '  \"details\": {'\necho '    \"outcome\":\"complete\"'\necho '  }'"
        status, result = self.run_adapter_start_and_wait(script)
        self.assertEqual((status["status"], result["outcome"], result["error_code"]), ("failed", "failure", "invalid_result"))

    def test_valid_terminal_result_after_earlier_malformed_object(self):
        status, result = self.run_adapter_start_and_wait("echo '{\"outcome\":\"blocked\"'\necho '{\"outcome\":\"complete\"}'")
        self.assertEqual((status["status"], result["outcome"]), ("complete", "complete"))

if __name__ == "__main__":
    unittest.main()
