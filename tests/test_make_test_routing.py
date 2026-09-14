"""Make test families remain independently selectable."""

from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MakeTestRoutingTests(unittest.TestCase):
    def dry_run(self, *targets):
        return subprocess.run(
            ["make", "-n", *targets], cwd=ROOT, check=True,
            text=True, stdout=subprocess.PIPE).stdout

    def test_core_does_not_run_embedding_suite(self):
        output = self.dry_run("core")
        self.assertIn("tests/run_core.py", output)
        self.assertNotIn("tests.test_embedding", output)

    def test_general_test_does_not_run_embedding_suite(self):
        output = self.dry_run("test")
        self.assertIn("tests/run.py", output)
        self.assertNotIn("tests.test_embedding", output)

    def test_embedding_suite_remains_explicit(self):
        output = self.dry_run("test-embedding")
        self.assertIn("tests.test_embedding", output)

    def test_install_ignores_editor_lock_modules(self):
        output = self.dry_run("install")
        self.assertIn('! -name ".#*"', output)

    def test_repl_target_runs_pipe_pty_cache_and_full_core_coverage(self):
        output = self.dry_run("repl")
        self.assertIn("tests.test_repl", output)
        self.assertIn("tests.test_repl_pty", output)
        self.assertIn("tests.test_repl_cache", output)
        # test_repl contains the one-session import of every core module.
        self.assertNotIn("tests/run.py", output)


if __name__ == "__main__":
    unittest.main()
