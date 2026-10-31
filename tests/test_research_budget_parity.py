"""Registered Codex and AgY attempts share the research budget contract."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

_SPEC = importlib.util.spec_from_file_location(
    'research_budget_runtime', Path(__file__).resolve().parents[1] / 'nightshift/worker_runtime.py')
runtime = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(runtime)


class ResearchBudgetParityTests(unittest.TestCase):
    def attempt_budget(self, adapter, title, profile=None, cli=None, environment=None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = argparse.Namespace(work_dir=temporary, task_id='TASK-BUDGET', title=title,
                step_budget=17, timeout=30, output_file=str(root/'output.json'),
                result_file=str(root/'result.json'))
            if cli is not None:
                args.research_step_budget = cli
            agent = dict(id='fixture', adapter=adapter, command=adapter, worker={}, overrides={})
            if profile is not None:
                agent['worker']['research_step_budget'] = profile
            observed = []
            def execute(attempt):
                observed.append(attempt.step_budget)
                Path(attempt.output_file).write_text(json.dumps({'status': 'complete'}))
                Path(attempt.result_file).write_text(json.dumps({'outcome': 'complete'}))
                return 0
            env = {} if environment is None else {'RESEARCH_STEP_BUDGET': str(environment)}
            with mock.patch.dict(os.environ, env, clear=True), \
                    mock.patch.object(runtime, 'capture_attempt_baseline'), \
                    mock.patch.object(runtime, 'execute_adapter_task', side_effect=execute):
                self.assertEqual(runtime.execute_registered_agents(args, [agent], root/'state.json',
                    lambda *_: self.fail('successful attempt must not roll back')), (0, False))
            return observed

    def test_default_research_budget_is_shared(self):
        for adapter in ('codex', 'agy'):
            with self.subTest(adapter=adapter):
                self.assertEqual(self.attempt_budget(adapter, 'TASK[RESEARCH]'), [100])

    def test_override_precedence_is_shared(self):
        for adapter in ('codex', 'agy'):
            for profile, cli, environment, expected in (
                    (140, 130, 120, 140), (None, 130, 120, 130),
                    (None, None, 120, 120), (None, 0, None, 100)):
                with self.subTest(adapter=adapter, profile=profile, cli=cli, env=environment):
                    self.assertEqual(self.attempt_budget(adapter, 'TASK[research]', profile, cli,
                                                         environment), [expected])

    def test_ordinary_and_body_only_tags_keep_code_budget(self):
        for adapter in ('codex', 'agy'):
            for title in ('TASK[CODE]', 'TASK | ACTION: mention [RESEARCH]'):
                with self.subTest(adapter=adapter, title=title):
                    self.assertEqual(self.attempt_budget(adapter, title, 140, 130, 120), [17])


if __name__ == '__main__':
    unittest.main()
