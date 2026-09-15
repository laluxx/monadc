"""Make test families remain independently selectable."""

from pathlib import Path
import importlib.machinery
import importlib.util
import sys
import unittest


ROOT = Path(__file__).resolve().parents[3]


def load_make_tool():
    loader = importlib.machinery.SourceFileLoader(
        "monadc_make_tar_contract", str(ROOT / "make"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class MakeTestRoutingTests(unittest.TestCase):
    def test_python_frontend_is_the_only_build_driver(self):
        self.assertTrue((ROOT / "make").exists())
        self.assertFalse((ROOT / "Makefile").exists())
        self.assertIn("COMMAND_SPECS", (ROOT / "make").read_text(encoding="utf-8"))

    def test_native_compile_plan_honors_parallel_jobs(self):
        frontend = (ROOT / "make").read_text(encoding="utf-8")
        self.assertIn("ThreadPoolExecutor", frontend)
        self.assertIn("max_workers=worker_count", frontend)

    def test_compact_short_jobs_option_is_supported(self):
        frontend = (ROOT / "make").read_text(encoding="utf-8")
        self.assertIn('arg.startswith("-j")', frontend)

    def test_source_archive_does_not_require_makefile(self):
        frontend = (ROOT / "make").read_text(encoding="utf-8")
        self.assertNotIn('    "Makefile",\n    "CMakeLists.txt",', frontend)
        self.assertIn('    "CMakeLists.txt",\n    "make",', frontend)

    def test_source_archive_accepts_generated_metadata_and_ast_goldens(self):
        make_tool = load_make_tool()

        # .mqti files are generated beside fixtures and checked-in .json files
        # are AST goldens; neither should make ./make tar reject the source tree.
        make_tool.validate_source_package_surface(ROOT)

    def test_core_does_not_run_embedding_suite(self):
        frontend = (ROOT / "make").read_text(encoding="utf-8")
        self.assertIn('run_python_module("src.testing.core_runner"', frontend)
        self.assertIn('"core"', frontend)

    def test_general_test_does_not_run_embedding_suite(self):
        frontend = (ROOT / "make").read_text(encoding="utf-8")
        self.assertIn('"src.testing.runner"', frontend)
        self.assertIn('"test"', frontend)

    def test_embedding_suite_remains_explicit(self):
        frontend = (ROOT / "make").read_text(encoding="utf-8")
        self.assertIn('"test_embedding*.py"', frontend)
        self.assertIn('"test-embedding"', frontend)

    def test_install_ignores_editor_lock_modules(self):
        frontend = (ROOT / "make").read_text(encoding="utf-8")
        self.assertIn('source.name.startswith(".#")', frontend)

    def test_repl_target_runs_pipe_pty_cache_and_full_core_coverage(self):
        frontend = (ROOT / "make").read_text(encoding="utf-8")
        for module in (
            "src.testing.contracts.test_repl",
            "src.testing.contracts.test_repl_pty",
            "src.testing.contracts.test_repl_cache",
        ):
            self.assertIn(module, frontend)

    def test_hooks_are_integrated_and_generated_hook_state_is_cleanable(self):
        frontend = (ROOT / "make").read_text(encoding="utf-8")
        pre_push = (ROOT / ".githooks" / "pre-push").read_text(encoding="utf-8")
        self.assertIn('".hooks"', frontend)
        self.assertIn('name.startswith(".monadc-test-bin-")', frontend)
        self.assertIn('".witnesses"', frontend)
        self.assertIn('"Show"', frontend)
        self.assertIn('"verify-push"', frontend)
        self.assertIn("--allow-failures", frontend)
        self.assertIn("MONAD_VERIFY_PUSH_ALLOW_FAILURES", pre_push)
        self.assertIn("exec ./make verify-push", pre_push)


if __name__ == "__main__":
    unittest.main()
