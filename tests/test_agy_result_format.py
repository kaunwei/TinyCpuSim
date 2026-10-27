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


if __name__ == "__main__":
    unittest.main()
