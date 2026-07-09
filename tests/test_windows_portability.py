import importlib.util
import importlib.machinery
import sys
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def read(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


def load_make_tool():
    loader = importlib.machinery.SourceFileLoader("monadc_make", str(ROOT / "make"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class WindowsPortabilityTests(unittest.TestCase):
    def test_make_tool_treats_msys2_as_windows_for_executables(self):
        make_tool = load_make_tool()

        with mock.patch.object(make_tool.platform, "system", return_value="MSYS_NT-10.0-22631"):
            self.assertEqual(make_tool.host_os(), "windows")

        with mock.patch.object(make_tool, "host_os", return_value="windows"):
            paths = [str(path) for path in make_tool.possible_binary_paths()]

        self.assertTrue(any(path.endswith("monad.exe") for path in paths))

    def test_makefile_has_windows_specific_link_and_target_contract(self):
        makefile = read("Makefile")

        self.assertIn("EXEEXT", makefile)
        self.assertIn("WINDOWS_HOST", makefile)
        self.assertIn("EXPORT_LDFLAG", makefile)
        self.assertIn("NO_PIE_LDFLAG", makefile)
        self.assertIn("$(TARGET_BASE)$(EXEEXT)", makefile)

    def test_compiler_link_paths_do_not_hardcode_posix_flags_for_windows(self):
        main_c = read("main.c")
        buildsystem_c = read("buildsystem.c")
        cli_c = read("cli.c")

        self.assertIn("host_exe_suffix", main_c)
        self.assertIn("host_no_pie_flag", main_c)
        self.assertIn("host_exe_suffix", buildsystem_c)
        self.assertIn("host_no_pie_flag", buildsystem_c)
        self.assertIn("host_exe_suffix", cli_c)
        self.assertIn("test_bin_name", cli_c)


if __name__ == "__main__":
    unittest.main()
