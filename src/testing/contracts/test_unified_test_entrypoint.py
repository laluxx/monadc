import subprocess
import unittest
from pathlib import Path

from src.testing.monad_binary import resolve_monad_binary

ROOT = Path(__file__).resolve().parents[3]
MONAD = resolve_monad_binary()


def read(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


class UnifiedTestEntrypointTests(unittest.TestCase):
    def test_host_suite_menu_is_module_based_and_human_named(self):
        result = subprocess.run(
            ["python3", "-B", "-m", "src.testing.suites", "list"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        for suite in ("runner", "core", "how-to", "windows"):
            self.assertIn(suite, result.stdout)

    def test_compiler_test_command_exposes_same_suite_menu(self):
        if not MONAD.exists():
            self.skipTest("compiler binary has not been built in this source-only checkout")
        result = subprocess.run(
            [str(MONAD), "test", "list"], cwd=ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        for suite in ("runner", "windows", "laws"):
            self.assertIn(suite, result.stdout)

    def test_compiler_test_help_is_discoverable(self):
        if not MONAD.exists():
            self.skipTest("compiler binary has not been built in this source-only checkout")
        result = subprocess.run(
            [str(MONAD), "test", "--help"], cwd=ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("monad test", result.stdout)
        self.assertIn("runner", result.stdout)
        self.assertIn("windows", result.stdout)

    def test_makefile_routes_tests_through_canonical_entrypoints(self):
        makefile = read("Makefile")
        self.assertIn("-m src.testing.runner", makefile)
        self.assertIn("$(TARGET_PATH) test runner", makefile)
        self.assertIn("-m src.testing.contracts.test_how_to_examples", makefile)
        self.assertNotIn("tests/test_", makefile)

    def test_runner_suite_is_declared_in_one_host_registry(self):
        suites = read("src/testing/suites.py")
        self.assertIn('"runner": Suite(', suites)
        self.assertIn('py("src.testing.contracts.test_run_core")', suites)
        self.assertIn('py("src.testing.contracts.test_repl")', suites)
        self.assertIn('py("src.testing.contracts.test_tail_calls")', suites)
        self.assertIn('py("src.testing.contracts.test_checkout_local_paths")', suites)
        self.assertIn('env["PYTHONUTF8"] = "1"', suites)

    def test_tests_tree_is_authored_monad_only(self):
        files = [p for p in (ROOT / "tests").rglob("*") if p.is_file()]
        self.assertTrue(files)
        non_monad = [p.relative_to(ROOT) for p in files if p.suffix != ".mon"]
        self.assertEqual(non_monad, [])

    def test_readme_advertises_public_test_front_doors_not_python_scripts(self):
        readme = read("README.md")
        self.assertIn("./make test", readme)
        self.assertIn("monad test list", readme)
        self.assertNotIn("python tests/main.py", readme)
        self.assertNotIn("tests/run.py", readme)


if __name__ == "__main__":
    unittest.main()
