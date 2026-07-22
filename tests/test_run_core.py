import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def load_core_runner():
    spec = importlib.util.spec_from_file_location("core_runner", ROOT / "tests" / "run_core.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class CoreRunnerDiscoveryTests(unittest.TestCase):
    def test_discovery_ignores_editor_lock_symlinks(self):
        runner_mod = load_core_runner()
        with tempfile.TemporaryDirectory() as td:
            core = Path(td) / "core"
            core.mkdir()
            real_module = core / "List.mon"
            real_module.write_text("(module List [])\n\ntests\n  assert-eq 1 1 \"sentinel\"\n", encoding="utf-8")
            (core / ".#List.mon").symlink_to("missing-lock-target")

            original_core_root = runner_mod.CORE_ROOT
            runner_mod.CORE_ROOT = core
            try:
                modules = runner_mod.discover_modules()
            finally:
                runner_mod.CORE_ROOT = original_core_root

        self.assertEqual(modules, [real_module])

    def test_materialize_requires_balanced_test_drawers(self):
        runner_mod = load_core_runner()
        with tempfile.TemporaryDirectory() as td:
            module = Path(td) / "Balanced.mon"
            module.write_text(
                "(module Balanced [])\n\n"
                "tests\n"
                "  :registration:\n"
                "  assert-eq 1 1 \"ok\"\n"
                "  :registration:\n",
                encoding="utf-8",
            )

            materialized = runner_mod.materialize_test_source(module)

        self.assertIn('(assert-eq 1 1 "ok")', materialized)

    def test_materialize_accepts_progress_cookie_on_tests_heading(self):
        runner_mod = load_core_runner()
        with tempfile.TemporaryDirectory() as td:
            module = Path(td) / "Cookie.mon"
            module.write_text(
                "(module Cookie [])\n\n"
                "tests [0/0]\n"
                "  assert-eq 1 1 \"one\"\n"
                "  assert-eq 2 2 \"two\"\n",
                encoding="utf-8",
            )

            materialized = runner_mod.materialize_test_source(module)

        self.assertIn('(assert-eq 1 1 "one")', materialized)
        self.assertIn('(assert-eq 2 2 "two")', materialized)

    def test_materialized_tree_runs_only_the_selected_modules_tests(self):
        runner_mod = load_core_runner()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            core = root / "core"
            output = root / "materialized"
            core.mkdir()
            active = core / "Active.mon"
            dependency = core / "Dependency.mon"
            active.write_text(
                "module Active\n\ntests\n  assert-eq 1 1 \"active\"\n",
                encoding="utf-8",
            )
            dependency.write_text(
                "module Dependency\n\ntests\n  assert-eq 2 2 \"dependency\"\n",
                encoding="utf-8",
            )

            original_core_root = runner_mod.CORE_ROOT
            runner_mod.CORE_ROOT = core
            try:
                runner_mod.materialize_core_tree(output, Path("Active.mon"))
            finally:
                runner_mod.CORE_ROOT = original_core_root

            active_text = (output / "Active.mon").read_text(encoding="utf-8")
            dependency_text = (output / "Dependency.mon").read_text(encoding="utf-8")

        self.assertIn('assert-eq 1 1 "active"', active_text)
        self.assertNotIn("tests", dependency_text)
        self.assertNotIn("dependency", dependency_text)

    def test_update_test_cookies_rewrites_existing_heading_counts(self):
        runner_mod = load_core_runner()
        with tempfile.TemporaryDirectory() as td:
            module = Path(td) / "Cookie.mon"
            module.write_text(
                "(module Cookie [])\n\n"
                "tests [0/0]\n"
                "  assert-eq 1 1 \"one\"\n"
                "  assert-eq 2 2 \"two\"\n",
                encoding="utf-8",
            )

            changed = runner_mod.update_test_cookies(module)
            text = module.read_text(encoding="utf-8")

        self.assertTrue(changed)
        self.assertIn("tests [2/2]", text)

    def test_update_test_cookies_leaves_plain_tests_heading_alone(self):
        runner_mod = load_core_runner()
        with tempfile.TemporaryDirectory() as td:
            module = Path(td) / "Plain.mon"
            original = (
                "(module Plain [])\n\n"
                "tests\n"
                "  assert-eq 1 1 \"one\"\n"
            )
            module.write_text(original, encoding="utf-8")

            changed = runner_mod.update_test_cookies(module)
            text = module.read_text(encoding="utf-8")

        self.assertFalse(changed)
        self.assertEqual(text, original)

    def test_materialize_rejects_unclosed_test_drawers(self):
        runner_mod = load_core_runner()
        with tempfile.TemporaryDirectory() as td:
            module = Path(td) / "Unbalanced.mon"
            module.write_text(
                "(module Unbalanced [])\n\n"
                "tests\n"
                "  :registration:\n"
                "  assert-eq 1 1 \"not closed\"\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "unclosed test drawer"):
                runner_mod.materialize_test_source(module)

    def test_active_core_modules_have_balanced_test_drawers(self):
        runner_mod = load_core_runner()
        for module in runner_mod.discover_modules():
            with self.subTest(module=str(module.relative_to(ROOT))):
                runner_mod.materialize_test_source(module)

    def test_active_core_modules_have_assertions(self):
        runner_mod = load_core_runner()
        for module in runner_mod.discover_modules():
            with self.subTest(module=str(module.relative_to(ROOT))):
                self.assertGreater(runner_mod.count_assertions(module), 0)


if __name__ == "__main__":
    unittest.main()
