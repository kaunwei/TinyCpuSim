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




class TaskKindPolicyTests(unittest.TestCase):
    def setUp(self):
        import tempfile
        from pathlib import Path
        self.temporary = tempfile.TemporaryDirectory(prefix="task-kind-policy-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name) / "project"
        self.work.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Kind Fixture")
        self.git("config", "user.email", "kind@example.invalid")
        (self.work / ".gitignore").write_text("__pycache__/\n")
        (self.work / "src").mkdir()
        (self.work / "tests").mkdir()
        (self.work / "src/a.py").write_text("value=1\n")
        (self.work / "tests/test_public.py").write_text(
            "import unittest\nclass PublicFixture(unittest.TestCase):\n"
            " def test_value(self):\n  self.assertEqual(1 + 1, 2)\n")
        self.git("add", ".")
        self.git("commit", "-qm", "baseline")
        self.base = self.git("rev-parse", "HEAD").stdout.strip()
        self.snapshot = self.work.parent / "snapshot"
        self.cli("attempt-snapshot", "--work-dir", self.work, "--task-id", "TASK-KIND",
                 "--attempt-id", "fixture", "--snapshot", self.snapshot)

    def run_command(self, *args):
        import subprocess
        return subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=10)

    def git(self, *args):
        result = self.run_command("git", "-C", self.work, *args)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def cli(self, *args):
        result = self.run_command("/usr/bin/python3", runtime.__file__, *args)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def change(self, path, content):
        target = self.work / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
        self.git("add", path)
        self.git("commit", "-qm", "task delta")

    def audit(self, title, constraints="", target="src/a.py", mechanical=True):
        args = ["--work-dir", self.work, "--base", self.base,
                "--baseline-snapshot", self.snapshot, "--title", title,
                "--constraints", constraints, "--target", target, "--level", "3"]
        if mechanical:
            result = self.cli("mechanical-audit", *args, "--task-id", "TASK-KIND",
                              "--verify", "/usr/bin/python3 -m unittest discover -s tests -p test_public.py -v",
                              "--verify-timeout", "5")
            data = json.loads(result.stdout)
            self.assertEqual(data["verify_exit"], 0, data)
            return data["status"]
        return self.run_command("/usr/bin/python3", runtime.__file__, "git-audit", *args).returncode

    def test_body_and_constraints_references_keep_ordinary_audits(self):
        self.change("src/a.py", "value=2\n")
        for tag in ("RESEARCH", "DIAGNOSTIC", "CODE: FIX", "CODE: REFACTOR"):
            for title, constraints in ((f"TASK-KIND | AFTER: TASK-PREV[{tag}]", ""),
                                       ("TASK-KIND", f"Reference TASK-PREV[{tag}]")):
                with self.subTest(tag=tag, title=title):
                    self.assertEqual(self.audit(title, constraints, mechanical=False), 0)
                    self.assertEqual(self.audit(title, constraints), 0)

    def test_body_research_tag_does_not_bypass_whitelist(self):
        self.change("src/undeclared.py", "value=2\n")
        for mechanical in (False, True):
            self.assertEqual(self.audit("TASK-KIND | AFTER: OTHER[RESEARCH]",
                                        mechanical=mechanical), 2)

    def test_genuine_research_and_diagnostic_restrictions_remain(self):
        self.change("src/a.py", "value=2\n")
        for title in ("TASK-KIND [RESEARCH] | ACTION: inspect",
                      "TASK-KIND[RESEARCH] | ACTION: inspect", "[RESEARCH] inspect"):
            with self.subTest(title=title):
                self.assertEqual(self.audit(title), 12)
        for mechanical in (False, True):
            self.assertEqual(self.audit("TASK-KIND [DIAGNOSTIC] | ACTION: reproduce",
                                        mechanical=mechanical), 4)

    def test_fix_header_keeps_test_immutability_but_body_reference_does_not(self):
        content = (self.work / "tests/test_public.py").read_text()
        self.change("tests/test_public.py", content + "# authorized test maintenance\n")
        for mechanical in (False, True):
            self.assertEqual(self.audit("TASK-KIND | AFTER: PREV[CODE: FIX]",
                                        target="tests/test_public.py", mechanical=mechanical), 0)
            self.assertEqual(self.audit("TASK-KIND [CODE: FIX] | ACTION: repair",
                                        target="tests/test_public.py", mechanical=mechanical), 5)

    def test_assertion_non_decrease_stays_enforced_despite_body_reference(self):
        self.change("tests/test_public.py", "import unittest\nclass PublicFixture(unittest.TestCase):\n def test_value(self):\n  pass\n")
        self.assertEqual(self.audit("TASK-KIND | ACTION: see PREV[RESEARCH]",
                                    target="tests/test_public.py", mechanical=False), 7)

    def test_fallback_commit_kind_uses_header_and_explicit_message_wins(self):
        from types import SimpleNamespace
        for title, explicit, expected in (
            ("TASK-KIND | AFTER: PREV[RESEARCH]", "", "feat(worker): complete TASK-KIND"),
            ("TASK-KIND [CODE: FIX] | ACTION: repair", "", "fix(worker): complete TASK-KIND"),
            ("TASK-KIND [CODE: REFACTOR] | ACTION: reshape", "", "refactor(worker): complete TASK-KIND"),
            ("TASK-KIND[RESEARCH] | ACTION: inspect", "", "docs(worker): complete TASK-KIND"),
            ("[DIAGNOSTIC] inspect", "", "test(worker): complete TASK-KIND"),
            ("TASK-KIND [RESEARCH] | ACTION: inspect", "chore: exact explicit subject", "chore: exact explicit subject"),
        ):
            with self.subTest(title=title, explicit=explicit):
                (self.work / "note.txt").write_text(title + explicit + "\n")
                args = SimpleNamespace(work_dir=str(self.work), target="note.txt", title=title,
                                       task_id="TASK-KIND", level=3, verify="", commit_message=explicit)
                self.assertEqual(runtime.finalize_task_git(args), 0)
                self.assertEqual(self.git("log", "-1", "--format=%s").stdout.strip(), expected)


if __name__ == "__main__":
    unittest.main()
