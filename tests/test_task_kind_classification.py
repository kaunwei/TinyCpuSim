import json
import unittest

from nightshift import worker_runtime as runtime


class TaskKindRoutingTests(unittest.TestCase):
    def setUp(self):
        self.research_agent = runtime.parse_agent_registry(json.dumps([{
            "id": "research-probe", "adapter": "generic", "command": "/bin/true",
            "overrides": {"research_only": True},
        }]))[0]

    def test_body_references_never_admit_code_to_research_agent(self):
        for field in ("AFTER", "ACTION", "VERIFY", "FORBIDDEN_SHORTCUTS"):
            with self.subTest(field=field):
                title = f"TASK-CODE | LEVEL: 3 | {field}: TASK-PREV[RESEARCH]"
                self.assertFalse(runtime.agent_supports_task(self.research_agent, title))

    def test_constraints_do_not_admit_code_to_research_agent(self):
        self.assertFalse(runtime.agent_supports_task(
            self.research_agent, "TASK-CODE | LEVEL: 3", "refer to TASK-PREV[RESEARCH]"))

    def test_real_research_headers_keep_supported_forms(self):
        for title in ("TASK-A [RESEARCH] | LEVEL: 3", "TASK-A[RESEARCH] | LEVEL: 3",
                      "[RESEARCH] inspect", "TASK-A [research] | ACTION: inspect"):
            with self.subTest(title=title):
                self.assertTrue(runtime.agent_supports_task(self.research_agent, title))

    def test_unrestricted_agent_still_accepts_ordinary_code(self):
        agent = runtime.parse_agent_registry(json.dumps([{
            "id": "code-probe", "adapter": "generic", "command": "/bin/true",
        }]))[0]
        self.assertTrue(runtime.agent_supports_task(agent, "TASK-CODE | LEVEL: 3"))

    def test_maintenance_requires_both_prefix_and_header_tag(self):
        cases = (
            ("SCRATCH-REVIEW-case [MAINTENANCE: SCRATCH] | ACTION: review", True),
            ("SCRATCH-REVIEW-case[MAINTENANCE: SCRATCH] | ACTION: review", True),
            ("SCRATCH-REVIEW-case | AFTER: OTHER[MAINTENANCE: SCRATCH]", False),
            ("SCRATCH-REVIEW-case | ACTION: use [MAINTENANCE: SCRATCH]", False),
            ("TASK-CODE [MAINTENANCE: SCRATCH] | ACTION: review", False),
            ("SCRATCH-REVIEW-case | ACTION: review", False),
        )
        for title, expected in cases:
            with self.subTest(title=title):
                self.assertEqual(runtime.scratch_review.is_review(title), expected)
                self.assertEqual(runtime.agent_supports_task(self.research_agent, title), expected)


if __name__ == "__main__":
    unittest.main()
