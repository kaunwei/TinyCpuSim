#!/usr/bin/env python3
"""unittest discovery and execution evidence for the hot hook protocol."""

import argparse
import ast
import hashlib
import inspect
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest


def methods(source):
    tree = ast.parse(source)
    found = {}
    for cls in tree.body:
        if isinstance(cls, ast.ClassDef):
            for node in cls.body:
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    # Docstrings/comments are not a substantive test change.
                    if node.body and isinstance(node.body[0], ast.Expr) and isinstance(node.body[0].value, ast.Constant) and isinstance(node.body[0].value.value, str):
                        node.body = node.body[1:]
                    found[(cls.name, node.name)] = hashlib.sha256(ast.dump(node, include_attributes=False).encode()).hexdigest()
    return found


def flatten(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from flatten(item)
        else:
            yield item


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--start", default="test")
    parser.add_argument("--pattern", default="test*.py")
    args = parser.parse_args()
    root = Path.cwd().resolve()
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    loader = unittest.TestLoader()
    suite = loader.discover(args.start, pattern=args.pattern)
    if loader.errors:
        raise RuntimeError("\n".join(loader.errors))
    tests = list(flatten(suite))
    cases = []
    for test in tests:
        source = Path(inspect.getfile(type(test))).resolve().relative_to(root).as_posix()
        method = test._testMethodName
        identity = (type(test).__name__, method)
        current = methods((root / source).read_text())[identity]
        baseline = subprocess.run(["git", "show", f"{os.environ['NIGHTSHIFT_BASE']}:{source}"], capture_output=True)
        previous = methods(baseline.stdout.decode()).get(identity) if baseline.returncode == 0 else None
        cases.append({"id": test.id(), "source": source, "fingerprint": current,
                      "baseline_fingerprint": previous})
    if os.environ["NIGHTSHIFT_PHASE"] == "run":
        class Result(unittest.TextTestResult):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, **kwargs)
                self.outcomes = {}

            def addSuccess(self, test):
                super().addSuccess(test)
                self.outcomes[test.id()] = "passed"

        result = unittest.TextTestRunner(resultclass=Result, verbosity=1).run(suite)
        for case in cases:
            case["status"] = result.outcomes.get(case["id"], "failed_or_skipped")
        code = 0 if result.wasSuccessful() and tests else 1
    else:
        code = 0
    Path(os.environ["NIGHTSHIFT_RESULT"]).write_text(json.dumps({"run": os.environ["NIGHTSHIFT_RUN"], "cases": cases}))
    return code


if __name__ == "__main__":
    sys.exit(main())
