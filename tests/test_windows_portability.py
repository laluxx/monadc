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

    def test_posix_only_headers_are_guarded_for_windows_builds(self):
        compat_h = read("compat.h")
        cli_c = read("cli.c")
        buildsystem_c = read("buildsystem.c")
        completion_c = read("completion.c")
        config_c = read("config.c")
        codegen_c = read("codegen.c")
        dep_c = read("dep.c")
        lsp_c = read("lsp.c")
        lsp_repl_c = read("lsp_repl.c")
        main_c = read("main.c")
        ffi_c = read("ffi.c")
        repl_c = read("repl.c")
        types_c = read("types.c")

        self.assertIn("monad_strndup", compat_h)
        self.assertIn("#define strndup monad_strndup", compat_h)
        self.assertIn("monad_mkdir", compat_h)
        self.assertIn("_mkdir", compat_h)
        self.assertIn("#if !defined(_WIN32)", cli_c)
        self.assertIn("host_system_success", cli_c)
        self.assertIn("host_mkdir", cli_c)
        self.assertIn("cli_strndup", cli_c)
        self.assertIn("host_self_path", cli_c)
        self.assertIn("GetModuleFileNameA", cli_c)
        self.assertIn("monad debug is not available on this Windows build", cli_c)
        self.assertIn('#include "compat.h"', codegen_c)
        self.assertIn("#if defined(_WIN32)", completion_c)
        self.assertIn("completion_menu_main", completion_c)
        self.assertIn("_mkdir", config_c)
        self.assertIn('#include "compat.h"', dep_c)
        self.assertIn("#if defined(_WIN32)", lsp_repl_c)
        self.assertIn("LSP REPL is not available on this Windows build", lsp_repl_c)
        self.assertIn("lsp_gmtime", lsp_c)
        self.assertIn("gmtime_s", lsp_c)
        self.assertIn("gmtime_r", lsp_c)
        self.assertIn("host_realpath", main_c)
        self.assertIn("_fullpath", main_c)
        self.assertIn("ensure_cache_dir", main_c)
        self.assertIn("monad_mkdir", main_c)
        self.assertIn("llvm_config_link_flags", main_c)
        self.assertNotIn("`llvm-config --ldflags --libs core`", main_c)
        self.assertNotIn("`llvm-config --ldflags --libs core`", buildsystem_c)
        self.assertNotIn("mkdir -p", main_c)
        self.assertIn("#if !defined(_WIN32)", ffi_c)
        self.assertIn('#include "compat.h"', ffi_c)
        self.assertIn("monad_mkdir", ffi_c)
        self.assertIn("#if !defined(_WIN32)", repl_c)
        self.assertIn('#include "compat.h"', repl_c)
        self.assertIn("#if defined(_WIN32)", repl_c)
        self.assertIn("signal(SIGSEGV, repl_signal_handler)", repl_c)
        self.assertIn("struct sigaction", repl_c)
        self.assertIn('#include "compat.h"', types_c)

    def test_debugger_header_and_cmake_are_windows_safe(self):
        debugger_h = read("debugger.h")
        cmake = read("CMakeLists.txt")

        self.assertIn("#if !defined(_WIN32)", debugger_h)
        self.assertIn("saved_termios", debugger_h)
        self.assertIn("if(WIN32)", cmake)
        self.assertIn("list(REMOVE_ITEM MONADC_COMPILER_SOURCES debugger.c)", cmake)


if __name__ == "__main__":
    unittest.main()
