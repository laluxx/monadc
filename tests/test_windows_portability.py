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
        self.assertIn("WINDOWS_EXCLUDED_SRCS", makefile)
        self.assertIn("debugger.c", makefile)
        self.assertIn("$(WINDOWS_EXCLUDED_SRCS)", makefile)
        self.assertIn("HEADERS = $(wildcard *.h)", makefile)
        self.assertIn("%.o: %.c $(HEADERS)", makefile)

    def test_cmake_gives_compiler_a_nontrivial_windows_stack(self):
        cmake = read("CMakeLists.txt")

        self.assertIn("if(WIN32)", cmake)
        self.assertIn('target_link_options(monad PRIVATE "-Wl,--stack,67108864")', cmake)

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

    def test_bulk_file_readers_are_binary_and_use_actual_read_size(self):
        main_c = read("main.c")
        cli_c = read("cli.c")

        self.assertIn('fopen(path, "rb")', main_c)
        self.assertIn("size_t n = fread(src, 1, sz, f);", main_c)
        self.assertIn("src[n] = '\\0';", main_c)
        self.assertNotIn("fread(src, 1, sz, f); src[sz] = '\\0';", main_c)

        self.assertIn('fopen(path, "rb")', cli_c)
        self.assertIn("size_t n = fread(buf, 1, sz, f);", cli_c)
        self.assertIn("buf[n] = '\\0';", cli_c)
        self.assertNotIn("fread(buf, 1, sz, f); buf[sz] = '\\0';", cli_c)

    def test_wisp_commentary_stripping_accepts_crlf_markers(self):
        wisp_c = read("wisp.c")

        self.assertIn("else if (*p == '\\r') break;", wisp_c)
        self.assertIn("bool blank = (trim >= line_end || *trim == '\\r');", wisp_c)
        self.assertIn("p == line_end || *p == '\\r' || *p == ' ' || *p == '\\t'", wisp_c)
        self.assertIn("*p == ' ' || *p == '\\t' || *p == '\\r'", wisp_c)
        self.assertIn("*p != ' ' && *p != '\\t' && *p != '\\r' && *p != ';'", wisp_c)
        self.assertIn("mt[14] == '\\r'", wisp_c)

    def test_core_char_test_labels_are_single_tokens(self):
        char_mon = read("core/prelude/Data/Char.mon")

        self.assertIn(":blanks-line-breaks:", char_mon)
        self.assertNotIn(":Blanks and line breaks:", char_mon)

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
        self.assertIn("if (*p == '/' || *p == '\\\\') *p = '_';", main_c)
        self.assertNotIn("`llvm-config --ldflags --libs core`", main_c)
        self.assertNotIn("`llvm-config --ldflags --libs core`", buildsystem_c)
        self.assertNotIn("mkdir -p", main_c)
        self.assertIn("#if !defined(_WIN32)", ffi_c)
        self.assertIn('#include "compat.h"', ffi_c)
        self.assertIn("monad_mkdir", ffi_c)
        self.assertIn("#if !defined(_WIN32)", repl_c)
        self.assertIn('#include "compat.h"', repl_c)
        self.assertIn("#if defined(_WIN32)", repl_c)
        self.assertIn("#define TokenType WindowsTokenType", repl_c)
        self.assertIn("#undef TokenType", repl_c)
        self.assertIn("signal(SIGSEGV, repl_signal_handler)", repl_c)
        self.assertIn("struct sigaction", repl_c)
        self.assertIn('#include "compat.h"', types_c)

    def test_repl_uses_orc_jit_not_mcjit(self):
        repl_c = read("repl.c")
        repl_h = read("repl.h")
        codegen_c = read("codegen.c")
        codegen_h = read("codegen.h")
        main_c = read("main.c")

        self.assertIn("#include <llvm-c/LLJIT.h>", repl_c)
        self.assertIn("#include <llvm-c/Transforms/PassBuilder.h>", repl_c)
        self.assertIn("#include <llvm-c/Transforms/PassBuilder.h>", codegen_c)
        self.assertIn("#include <llvm/Config/llvm-config.h>", repl_c)
        self.assertIn("#include <llvm/Config/llvm-config.h>", codegen_c)
        self.assertIn("LLVMOrcCreateLLJIT", repl_c)
        self.assertIn("LLVMOrcLLJITAddLLVMIRModule", repl_c)
        self.assertIn("LLVMOrcLLJITLookup", repl_c)
        self.assertIn("LLVMOrcCreateLLJIT", codegen_c)
        self.assertIn("#if LLVM_VERSION_MAJOR >= 19", repl_c)
        self.assertIn("#if LLVM_VERSION_MAJOR >= 19", codegen_c)
        self.assertIn("LLVMOrcCreateNewThreadSafeContext()", repl_c)
        self.assertIn("LLVMOrcCreateNewThreadSafeContext()", codegen_c)
        self.assertIn("LLVMOrcLLJITGetDataLayoutStr", repl_c)
        self.assertIn("LLVMSetDataLayout", repl_c)
        self.assertIn("LLVMSetDataLayout", codegen_c)
        self.assertIn("LLVMRunPasses", repl_c)
        self.assertIn("LLVMRunPasses", codegen_c)
        self.assertIn("default<O0>", repl_c)
        self.assertIn("default<O0>", codegen_c)
        self.assertNotIn("LLVMCreateExecutionEngineForModule", repl_c + codegen_c)
        self.assertNotIn("LLVMLinkInMCJIT", repl_c + codegen_c)
        self.assertNotIn("#include <llvm-c/ExecutionEngine.h>", repl_c + repl_h + codegen_c + codegen_h + main_c)

    def test_orc_runtime_symbols_are_registered_without_dlsym(self):
        repl_c = read("repl.c")

        start = repl_c.index("static void rt_sym_table_init")
        end = repl_c.index("#undef ADD", start)
        table_init = repl_c[start:end]

        self.assertIn("(void *)(uintptr_t)&n", table_init)
        self.assertNotIn("dlsym(RTLD_DEFAULT", table_init)
        self.assertIn("ADD(rt_list_empty)", table_init)
        self.assertIn("ADD(rt_list_cons)", table_init)
        self.assertIn("ADD(printf)", table_init)

    def test_all_build_paths_link_orc_jit_components(self):
        makefile = read("Makefile")
        main_c = read("main.c")
        buildsystem_c = read("buildsystem.c")

        self.assertIn("LLVM_COMPONENTS = core orcjit native passes", makefile)
        self.assertIn("llvm-config --ldflags --libs $(LLVM_COMPONENTS)", makefile)
        self.assertIn("llvm-config --ldflags --libs core orcjit native passes", main_c)
        self.assertIn("llvm-config --ldflags --libs core orcjit native passes", buildsystem_c)

    def test_debugger_header_and_cmake_are_windows_safe(self):
        debugger_h = read("debugger.h")
        cmake = read("CMakeLists.txt")

        self.assertIn("#if !defined(_WIN32)", debugger_h)
        self.assertIn("saved_termios", debugger_h)
        self.assertIn("if(WIN32)", cmake)
        self.assertIn("list(REMOVE_ITEM MONADC_COMPILER_SOURCES debugger.c)", cmake)

    def test_python_test_harnesses_resolve_windows_compiler_binary(self):
        helper = read("tests/monad_binary.py")
        tuple_test = read("tests/test_tuple_commas.py")
        run_core = read("tests/run_core.py")

        self.assertIn("MONAD_BINARY", helper)
        self.assertIn("monad.exe", helper)
        self.assertIn("resolve_monad_binary", tuple_test)
        self.assertIn("resolve_monad_binary", run_core)


if __name__ == "__main__":
    unittest.main()
