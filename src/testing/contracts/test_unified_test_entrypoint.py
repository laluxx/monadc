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

    def test_python_build_driver_routes_tests_through_canonical_entrypoints(self):
        make_driver = read("make")
        self.assertIn('"-m", "src.testing.runner"', make_driver)
        self.assertIn('"-m", "src.testing.suites", "runner"', make_driver)
        self.assertIn("src/testing/runner.py", make_driver)
        self.assertIn('"--no-contracts" not in args', make_driver)
        self.assertIn('parser.add_argument("--glyphs", default=None)', make_driver)
        self.assertIn('("--profile", "Record compiler and fixture subprocess timings.")', make_driver)
        self.assertNotIn("tests/test_", make_driver)

    def test_contracts_have_a_distinct_terminal_mark(self):
        console = read("src/tooling/console.py")
        suites = read("src/testing/suites.py")
        self.assertIn('"contract": ("◇", "C")', console)
        self.assertIn("UI.mark('contract')", suites)

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
        # ``.mqti`` is the runner's ignored, generated metadata cache; it is
        # not an authored test sidecar and may exist after ``./make test``.
        non_monad = [p.relative_to(ROOT) for p in files if p.suffix not in {".mon", ".mqti"}]
        self.assertEqual(non_monad, [])

    def test_readme_advertises_public_test_front_doors_not_python_scripts(self):
        readme = read("README.md")
        self.assertIn("./make test", readme)
        self.assertIn("monad test list", readme)
        self.assertNotIn("python tests/main.py", readme)
        self.assertNotIn("tests/run.py", readme)


if __name__ == "__main__":
    unittest.main()
