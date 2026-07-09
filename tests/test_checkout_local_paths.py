from pathlib import Path
import os
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]
MONAD = Path(os.environ.get("MONAD_BINARY", ROOT / "monad"))


def read(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


class CheckoutLocalPathTests(unittest.TestCase):
    def test_compiler_discovers_checkout_core_and_runtime_archive(self):
        main_c = read("main.c")

        self.assertIn("monad_core_dir", main_c)
        self.assertIn("runtime_archive_path", main_c)
        self.assertIn("MONAD_RUNTIME_LIB", main_c)
        self.assertIn('"core"', main_c)
        self.assertIn('"libmonad.a"', main_c)
        self.assertNotIn(" -o %s /usr/local/lib/libmonad.a", main_c)

    def test_legacy_buildsystem_links_runtime_archive_not_object_file(self):
        buildsystem_c = read("buildsystem.c")

        self.assertIn("build_runtime_archive_path", buildsystem_c)
        self.assertIn("MONAD_RUNTIME_LIB", buildsystem_c)
        self.assertIn('"libmonad.a"', buildsystem_c)
        self.assertNotIn('strcat(cmd, "runtime.o -o ");', buildsystem_c)

    def test_repl_module_import_link_uses_runtime_archive_resolver(self):
        repl_c = read("repl.c")

        self.assertIn("repl_runtime_archive_path", repl_c)
        self.assertIn("MONAD_RUNTIME_LIB", repl_c)
        self.assertIn('"libmonad.a"', repl_c)
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
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)

            run_result = subprocess.run(
                [str(output)],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(run_result.returncode, 0, run_result.stdout)
            self.assertTrue(run_result.stdout.endswith("42\n"), run_result.stdout)

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
            self.assertEqual(build_result.returncode, 0, build_result.stdout)

            exe = project / "build" / "checkout-smoke"
            if os.name == "nt":
                exe = exe.with_suffix(".exe")
            run_result = subprocess.run(
                [str(exe)],
                cwd=project,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(run_result.returncode, 0, run_result.stdout)
            self.assertTrue(run_result.stdout.endswith("42\n"), run_result.stdout)


if __name__ == "__main__":
    unittest.main()
