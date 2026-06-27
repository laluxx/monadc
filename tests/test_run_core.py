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
