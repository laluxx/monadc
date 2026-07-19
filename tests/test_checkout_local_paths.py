from pathlib import Path
import os
import re
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]
WINDOWS_DRIVE_PATH = re.compile(r"^[A-Za-z]:[\\/]")


def github_escape(value: str) -> str:
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def summarize_for_github_annotation(message: str, limit: int = 3500) -> str:
    if len(message) <= limit:
        return message
    marker = "\n...\n[truncated: keeping failure tail]\n...\n"
    head_limit = min(900, max(0, limit // 3))
    tail_limit = max(0, limit - head_limit - len(marker))
    tail = message[-tail_limit:] if tail_limit else ""
    return message[:head_limit] + marker + tail


def emit_github_error(title: str, message: str) -> None:
    if os.environ.get("GITHUB_ACTIONS") != "true":
        return
    body = github_escape(summarize_for_github_annotation(message))
    print(f"::error title={github_escape(title)}::{body}", flush=True)


def resolve_monad_binary(value: str | os.PathLike[str] | None = None) -> Path:
    raw = value or os.environ.get("MONAD_BINARY", ROOT / "monad")
    path = Path(raw)
    if not path.is_absolute() and not WINDOWS_DRIVE_PATH.match(str(raw)):
        path = ROOT / path
    return path


MONAD = resolve_monad_binary()


def read(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


def generated_executable(path: Path) -> Path:
    exe_path = Path(str(path) + ".exe")
    if exe_path.exists():
        return exe_path
    return path


class CheckoutLocalPathTests(unittest.TestCase):
    def test_relative_monad_binary_env_resolves_from_checkout_root(self):
        self.assertEqual(resolve_monad_binary("build/monad"), ROOT / "build" / "monad")

    def test_windows_drive_monad_binary_env_is_already_absolute(self):
        self.assertEqual(resolve_monad_binary("D:/a/monadc/monadc/build/monad.exe"),
                         Path("D:/a/monadc/monadc/build/monad.exe"))

    def test_github_annotations_keep_failure_tail(self):
        message = "header\n" + ("warning line\n" * 400) + "final linker error\n"
        summarized = summarize_for_github_annotation(message, limit=200)
        self.assertLessEqual(len(summarized), 200)
        self.assertIn("header", summarized)
        self.assertIn("truncated: keeping failure tail", summarized)
        self.assertTrue(summarized.endswith("final linker error\n"), summarized)

    def test_generated_executable_prefers_windows_suffix_when_present(self):
        with tempfile.TemporaryDirectory(prefix="monadc-exe-suffix-") as td:
            output = Path(td) / "hello"
            exe = Path(str(output) + ".exe")
            exe.touch()
            self.assertEqual(generated_executable(output), exe)

    def test_compiler_discovers_checkout_core_and_runtime_archive(self):
        main_c = read("main.c")

        self.assertIn("monad_core_dir", main_c)
        self.assertIn("runtime_archive_path", main_c)
        self.assertIn("MONAD_RUNTIME_LIB", main_c)
        self.assertIn('"core"', main_c)
        self.assertIn('"libmonad.a"', main_c)
        self.assertNotIn(" -o %s /usr/local/lib/libmonad.a", main_c)

    def test_core_subr_manifest_precedes_prelude_discovery(self):
        main_c = read("main.c")
        subr = read("core/Subr.mon")

        self.assertIn("module Subr", subr)
        self.assertIn("compile_core_subr", main_c)
        self.assertLess(
            main_c.index("compile_core_subr(current_source"),
            main_c.index("compile_prelude_dir(path, current_source"),
        )

    def test_legacy_buildsystem_links_runtime_archive_not_object_file(self):
        buildsystem_c = read("buildsystem.c")

        self.assertIn("build_runtime_archive_path", buildsystem_c)
        self.assertIn("MONAD_RUNTIME_LIB", buildsystem_c)
        self.assertIn('"libmonad.a"', buildsystem_c)
        self.assertNotIn('strcat(cmd, "runtime.o -o ");', buildsystem_c)

    def test_standalone_error_handlers_are_strong_separate_archive_members(self):
        cmake = read("CMakeLists.txt")
        makefile = read("Makefile")
        runtime_c = read("runtime.c")
        runtime_errors_c = read("runtime_errors.c")

        self.assertIn("runtime_errors.c", cmake)
        self.assertIn("runtime_errors.c", makefile)
        self.assertNotIn("void __monad_runtime_error(", runtime_c)
        self.assertNotIn("__attribute__((weak))", runtime_errors_c)
        self.assertIn("void __monad_runtime_error(", runtime_errors_c)
        self.assertIn("void __monad_assert_fail(", runtime_errors_c)

    def test_package_build_shell_quotes_compiler_and_project_paths(self):
        cli_c = read("cli.c")

        self.assertIn("shell_quote_arg(self", cli_c)
        self.assertIn("shell_quote_arg(bi->main_file", cli_c)
        self.assertIn("shell_quote_arg(bi->out_path", cli_c)
        self.assertIn("quoted_self, quoted_main, quoted_out", cli_c)
        self.assertIn("Windows cmd.exe requires an outer quote", cli_c)
        self.assertIn('"\\\"%s %s -o %s%s%s%s%s%s\\\""', cli_c)

    def test_repl_module_import_link_uses_runtime_archive_resolver(self):
        repl_c = read("repl.c")

        self.assertIn("repl_runtime_archive_path", repl_c)
        self.assertIn("MONAD_RUNTIME_LIB", repl_c)
        self.assertIn('"libmonad.a"', repl_c)
        self.assertIn('popen("llvm-config --ldflags --libs core", "r")', repl_c)
        self.assertNotIn("`llvm-config --ldflags --libs core`", repl_c)
        self.assertIn('" \\"%s\\" %s -lm -lgmp 2>&1"', repl_c)
        self.assertIn('"%s__mrepl_%ld_%s.dll"', repl_c)
        self.assertIn('"/tmp/__mrepl_%ld_%s.so"', repl_c)
        self.assertIn("(long)getpid(), mod_name", repl_c)
        self.assertNotIn(" /usr/local/lib/libmonad.a -lm 2>&1", repl_c)

    def test_checkout_compiler_can_compile_program_without_install_env(self):
        with tempfile.TemporaryDirectory(prefix="monadc-local-checkout-") as td:
            temp = Path(td)
            home = temp / "home"
            home.mkdir()
            source = temp / "hello.mon"
            output = temp / "hello"
            source.write_text(textwrap.dedent("""
                show 42
            """).strip() + "\n", encoding="utf-8")

            env = os.environ.copy()
            env["HOME"] = str(home)
            env.pop("MONAD_CORE", None)
            env.pop("MONAD_RUNTIME_LIB", None)

            compile_result = subprocess.run(
                [str(MONAD), str(source), "-o", str(output)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            if compile_result.returncode != 0:
                emit_github_error(
                    "checkout compiler smoke compile failed",
                    f"MONAD={MONAD}\nsource={source}\noutput={output}\n{compile_result.stdout}",
                )
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)

            exe = generated_executable(output)
            run_result = subprocess.run(
                [str(exe)],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            if run_result.returncode != 0:
                emit_github_error(
                    "checkout compiler smoke executable failed",
                    f"executable={exe}\n{run_result.stdout}",
                )
            self.assertEqual(run_result.returncode, 0, run_result.stdout)
            self.assertTrue(run_result.stdout.endswith("42\n"), run_result.stdout)

    def test_checkout_compiler_can_compile_core_module_with_imports(self):
        with tempfile.TemporaryDirectory(prefix="monadc-local-core-module-") as td:
            temp = Path(td)
            home = temp / "home"
            output = temp / "Distributive.o"
            home.mkdir()

            env = os.environ.copy()
            env["HOME"] = str(home)
            env.pop("MONAD_CORE", None)
            env.pop("MONAD_RUNTIME_LIB", None)

            result = subprocess.run(
                [str(MONAD), "--emit-obj",
                 str(ROOT / "core/prelude/Data/Distributive.mon"),
                 "-o", str(output)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertTrue(generated_executable(output).exists(), result.stdout)

    def test_compiler_returns_failure_when_linker_fails(self):
        with tempfile.TemporaryDirectory(prefix="monadc-link-failure-") as td:
            temp = Path(td)
            source = temp / "main.mon"
            output = temp / "main"
            source.write_text("(module Main)\nshow 42\n", encoding="utf-8")

            env = os.environ.copy()
            env["HOME"] = str(temp)
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(temp / "missing-libmonad.a")
            result = subprocess.run(
                [str(MONAD), str(source), "-o", str(output)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("linking failed", result.stdout)

    def test_package_build_finds_checkout_core_from_project_directory(self):
        with tempfile.TemporaryDirectory(prefix="monadc-package-checkout-") as td:
            project = Path(td)
            home = project / "home"
            src = project / "src"
            home.mkdir()
            src.mkdir()

            (project / "package.yaml").write_text(textwrap.dedent("""
                name: checkout-smoke
                executables:
                  checkout-smoke:
                    main: Main.mon
                    source-dirs: src
            """).strip() + "\n", encoding="utf-8")
            (src / "Main.mon").write_text(textwrap.dedent("""
                (module Main)
                show 42
            """).strip() + "\n", encoding="utf-8")

            env = os.environ.copy()
            env["HOME"] = str(home)
            env.pop("MONAD_CORE", None)
            env.pop("MONAD_RUNTIME_LIB", None)

            build_result = subprocess.run(
                [str(MONAD), "build"],
                cwd=project,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            if build_result.returncode != 0:
                emit_github_error(
                    "checkout package smoke build failed",
                    f"MONAD={MONAD}\nproject={project}\n{build_result.stdout}",
                )
            self.assertEqual(build_result.returncode, 0, build_result.stdout)
            self.assertNotIn("[dep] Warning:", build_result.stdout)
            self.assertNotIn("Class:", build_result.stdout)
            self.assertNotIn("compiled method:", build_result.stdout)

            exe = generated_executable(project / "build" / "checkout-smoke")
            run_result = subprocess.run(
                [str(exe)],
                cwd=project,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            if run_result.returncode != 0:
                emit_github_error(
                    "checkout package smoke executable failed",
                    f"executable={exe}\n{run_result.stdout}",
                )
            self.assertEqual(run_result.returncode, 0, run_result.stdout)
            self.assertTrue(run_result.stdout.endswith("42\n"), run_result.stdout)


if __name__ == "__main__":
    unittest.main()
