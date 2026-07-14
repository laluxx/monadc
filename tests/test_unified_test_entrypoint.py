import subprocess
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary

ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()


def read(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


class UnifiedTestEntrypointTests(unittest.TestCase):
    def test_test_main_lists_human_named_suites(self):
        result = subprocess.run(
            ["python3", "tests/main.py", "list"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("runner", result.stdout)
        self.assertIn("core", result.stdout)
        self.assertIn("how-to", result.stdout)
        self.assertIn("windows", result.stdout)

    def test_compiler_test_command_exposes_same_suite_menu(self):
        result = subprocess.run(
            [str(MONAD), "test", "list"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Available test suites:", result.stdout)
        self.assertIn("runner", result.stdout)
        self.assertIn("windows", result.stdout)

    def test_compiler_test_help_is_the_discoverable_front_door(self):
        result = subprocess.run(
            [str(MONAD), "test", "--help"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Usage: monad test [list|runner|windows|how-to|file.mon]", result.stdout)
        self.assertIn("Available test suites:", result.stdout)
        self.assertIn("runner", result.stdout)
        self.assertIn("windows", result.stdout)

    def test_makefile_routes_ad_hoc_runner_suite_through_single_entrypoint(self):
        makefile = read("Makefile")

        self.assertIn("./$(TARGET) test runner", makefile)
        self.assertIn("./$(TARGET) test how-to", makefile)
        self.assertNotIn("$(PYTHON) tests/test_tuple_commas.py", makefile)

    def test_runner_suite_covers_core_runner_contracts(self):
        test_main = read("tests/main.py")

        self.assertIn('py("tests/test_run_core.py")', test_main)

    def test_readme_advertises_unified_test_entrypoint(self):
        readme = read("README.md")

        self.assertIn("python tests/main.py list", readme)
        self.assertIn("python tests/main.py runner", readme)
        self.assertIn("monad test list", readme)
        self.assertIn("monad test runner", readme)


if __name__ == "__main__":
    unittest.main()
