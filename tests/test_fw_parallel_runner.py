import json
import os
import pathlib
import sys
import tempfile
import unittest

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from scripts.run_parallel_tests import (
    DEFAULT_CONFIG_BUDGET,
    ConfigResult,
    RunnerSummary,
    main,
    resolve_config_command,
    run_parallel_suite,
    run_single_config,
)
from tests.fw_resource_support import ResourceBudget, ResourcePool


class TestFwParallelRunner(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.scratch_root = pathlib.Path(self.temp_dir.name)

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_resolve_config_command_returns_valid_vectors(self) -> None:
        cmd = resolve_config_command("debug", self.scratch_root)
        self.assertIsInstance(cmd, list)
        self.assertIn("unittest", cmd)
        self.assertEqual(cmd[0], "/usr/bin/python3" if os.path.exists("/usr/bin/python3") else sys.executable)

        custom_cmd = resolve_config_command("custom_unknown", self.scratch_root)
        self.assertIn("-c", custom_cmd)

    def test_run_single_config_success(self) -> None:
        pool = ResourcePool(
            directory=self.scratch_root / "pool",
            budget=ResourceBudget(cpu_slots=2, memory_mib=1024),
        )
        py = "/usr/bin/python3" if os.path.exists("/usr/bin/python3") else sys.executable
        custom_cmd = [py, "-c", "import sys; print('HELLO'); sys.exit(0)"]
        res = run_single_config(
            config="test_cfg",
            scratch_root=self.scratch_root,
            pool=pool,
            timeout=10.0,
            budget=DEFAULT_CONFIG_BUDGET,
            custom_cmd=custom_cmd,
        )
        self.assertEqual(res.exit_code, 0)
        self.assertIn("HELLO", res.stdout)
        self.assertGreaterEqual(res.elapsed_seconds, 0.0)

    def test_run_single_config_failure_propagation(self) -> None:
        py = "/usr/bin/python3" if os.path.exists("/usr/bin/python3") else sys.executable
        custom_cmd = [py, "-c", "import sys; sys.stderr.write('ERR'); sys.exit(42)"]
        res = run_single_config(
            config="fail_cfg",
            scratch_root=self.scratch_root,
            pool=None,
            timeout=10.0,
            budget=DEFAULT_CONFIG_BUDGET,
            custom_cmd=custom_cmd,
        )
        self.assertEqual(res.exit_code, 42)
        self.assertIn("ERR", res.stderr)

    def test_run_single_config_timeout_handling(self) -> None:
        py = "/usr/bin/python3" if os.path.exists("/usr/bin/python3") else sys.executable
        custom_cmd = [py, "-c", "import time; time.sleep(5)"]
        res = run_single_config(
            config="timeout_cfg",
            scratch_root=self.scratch_root,
            pool=None,
            timeout=0.5,
            budget=DEFAULT_CONFIG_BUDGET,
            custom_cmd=custom_cmd,
        )
        self.assertEqual(res.exit_code, 124)
        self.assertIn("TIMEOUT", res.stderr)

    def test_run_parallel_suite_concurrent_execution(self) -> None:
        configs = ["c1", "c2", "c3"]
        py = "/usr/bin/python3" if os.path.exists("/usr/bin/python3") else sys.executable
        custom_cmd = [py, "-c", "import time; time.sleep(0.1); print('DONE')"]
        summary = run_parallel_suite(
            configs=configs,
            jobs=3,
            memory_mb=2048,
            scratch_root=self.scratch_root,
            timeout=10.0,
            sequential=False,
            budget=DEFAULT_CONFIG_BUDGET,
            custom_cmd=custom_cmd,
        )
        self.assertEqual(summary.total_configs, 3)
        self.assertEqual(summary.passed_configs, 3)
        self.assertEqual(summary.failed_configs, 0)
        self.assertEqual(len(summary.results), 3)
        for r in summary.results:
            self.assertEqual(r.exit_code, 0)
            self.assertIn("DONE", r.stdout)

    def test_run_parallel_suite_sequential_mode(self) -> None:
        configs = ["seq1", "seq2"]
        py = "/usr/bin/python3" if os.path.exists("/usr/bin/python3") else sys.executable
        custom_cmd = [py, "-c", "print('SEQ')"]
        summary = run_parallel_suite(
            configs=configs,
            jobs=1,
            memory_mb=1024,
            scratch_root=self.scratch_root,
            timeout=10.0,
            sequential=True,
            budget=DEFAULT_CONFIG_BUDGET,
            custom_cmd=custom_cmd,
        )
        self.assertTrue(summary.sequential)
        self.assertEqual(summary.passed_configs, 2)
        self.assertEqual(summary.failed_configs, 0)

    def test_main_cli_json_and_exit_code(self) -> None:
        argv = [
            "--configs", "quick1", "quick2",
            "--jobs", "2",
            "--scratch-root", str(self.scratch_root / "cli_scratch"),
            "--json",
        ]
        ret = main(argv)
        self.assertEqual(ret, 0)


if __name__ == "__main__":
    unittest.main()
