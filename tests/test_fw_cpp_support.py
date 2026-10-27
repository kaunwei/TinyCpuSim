import subprocess
import unittest
from tests.fw_cpp_test_support import run_cpp


class TestFwCppSupport(unittest.TestCase):
    def test_run_cpp_success_and_exit_code(self):
        source = """#include <iostream>

int main() {
    std::cout << "ok" << std::endl;
    return 7;
}
"""
        proc = run_cpp(source)
        self.assertIn("ok", proc.stdout)
        self.assertEqual(proc.returncode, 7)

    def test_run_cpp_syntax_error(self):
        source = """#include <iostream>

int main() {
    this is a syntax error!
    return 0;
}
"""
        with self.assertRaises(subprocess.CalledProcessError) as ctx:
            run_cpp(source)
        self.assertTrue(
            "syntax error" in ctx.exception.stderr or "error:" in ctx.exception.stderr or ctx.exception.returncode != 0
        )
        self.assertTrue(len(ctx.exception.stderr) > 0 or len(ctx.exception.stdout) > 0)

    def test_run_cpp_timeout_expired(self):
        source = """int main() {
    while (true) {}
    return 0;
}
"""
        with self.assertRaises(subprocess.TimeoutExpired):
            run_cpp(source, timeout=0.2)


if __name__ == "__main__":
    unittest.main()
