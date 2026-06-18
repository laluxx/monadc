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


if __name__ == "__main__":
    unittest.main()
