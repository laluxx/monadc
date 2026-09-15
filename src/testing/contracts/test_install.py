"""Installation paths and privilege handoff stay explicit and testable."""

import importlib.machinery
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]


def load_make_tool():
    loader = importlib.machinery.SourceFileLoader("monadc_make_install", str(ROOT / "make"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class InstallContractTests(unittest.TestCase):
    def test_local_install_layout_is_checkout_relative(self):
        make_tool = load_make_tool()

        paths = make_tool.install_paths("local")

        self.assertEqual(paths.prefix, ROOT / "local")
        self.assertEqual(paths.bindir, ROOT / "local" / "bin")
        self.assertEqual(paths.libdir, ROOT / "local" / "lib")
        self.assertEqual(paths.incdir, ROOT / "local" / "include" / "monad")
        self.assertEqual(paths.core_dir, ROOT / "local" / "lib" / "monad" / "core")

    def test_full_install_handoff_reexecutes_frontend_under_sudo(self):
        make_tool = load_make_tool()

        command = make_tool.sudo_install_command()

        self.assertEqual(command[:2], ["sudo", sys.executable])
        self.assertEqual(command[-2:], ["install", "--_privileged"])
        self.assertEqual(command[-3], str(ROOT / "make"))

    def test_local_install_does_not_require_elevation(self):
        make_tool = load_make_tool()
        paths = make_tool.install_paths("local")
        self.assertFalse(make_tool.install_needs_elevation(paths))

    def test_local_install_selects_local_tree_without_sudo(self):
        make_tool = load_make_tool()
        fake_target = Path(tempfile.gettempdir()) / "monadc-install-test-binary"
        with (mock.patch.object(make_tool, "build_project", return_value=fake_target) as build,
              mock.patch.object(make_tool, "_install_artifacts", return_value=0) as install,
              mock.patch.object(make_tool.subprocess, "run") as run):
            self.assertEqual(make_tool.command_install(["local"], SimpleNamespace(jobs=1)), 0)
        build.assert_called_once_with("debug", jobs=1)
        install.assert_called_once()
        run.assert_not_called()

    def test_system_install_builds_before_handing_off_to_sudo(self):
        make_tool = load_make_tool()
        fake_target = Path(tempfile.gettempdir()) / "monadc-install-test-binary"
        completed = SimpleNamespace(returncode=0)
        with (mock.patch.object(make_tool, "build_project", return_value=fake_target) as build,
              mock.patch.object(make_tool, "install_needs_elevation", return_value=True),
              mock.patch.object(make_tool.shutil, "which", return_value="/usr/bin/sudo"),
              mock.patch.object(make_tool.subprocess, "run", return_value=completed) as run):
            self.assertEqual(make_tool.command_install([], SimpleNamespace(jobs=1)), 0)
        build.assert_called_once_with("debug", jobs=1)
        handed_off = run.call_args.args[0]
        self.assertEqual(handed_off[4:6], ["install", "--_privileged"])
        self.assertTrue(any(arg.startswith("--_prefix=") for arg in handed_off))

    def test_install_help_documents_local_mode_and_sudo(self):
        result = subprocess.run(
            [str(ROOT / "make"), "--no-color", "help", "install"],
            cwd=ROOT,
            env={**os.environ, "TERM": "dumb"},
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("./make install [local]", result.stdout)
        self.assertIn("sudo", result.stdout.lower())

    def test_verify_push_can_explicitly_report_but_not_block_failures(self):
        make_tool = load_make_tool()
        with (mock.patch.object(make_tool, "command_clean", return_value=0),
              mock.patch.object(make_tool, "command_check", return_value=1),
              mock.patch.object(make_tool.UI, "warn")):
            self.assertEqual(
                make_tool.command_verify_push(["--allow-failures"], SimpleNamespace()),
                0,
            )


if __name__ == "__main__":
    unittest.main()
