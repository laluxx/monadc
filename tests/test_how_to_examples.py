import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary, resolve_runtime_archive


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()
RUNTIME = resolve_runtime_archive(MONAD)


class HowToExampleTests(unittest.TestCase):
    EXAMPLES = (
        "how_to/Syntax.mon",
        "how_to/AlgebraicDataTypes.mon",
        "how_to/Macros.mon",
        "how_to/Iter.mon",
        "how_to/QuickCheck.mon",
    )

    def test_readme_listed_how_to_examples_compile(self):
        for example in self.EXAMPLES:
            with self.subTest(example=example):
                with tempfile.TemporaryDirectory(prefix="monadc-howto-home-") as home:
                    output = Path(home) / Path(example).stem
                    env = os.environ.copy()
                    env["HOME"] = home
                    env["MONAD_CORE"] = str(ROOT / "core")
                    env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
                    result = subprocess.run(
                        [str(MONAD), str(ROOT / example), "-o", str(output)],
                        cwd=ROOT,
                        env=env,
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        check=False,
                    )

                self.assertEqual(
                    result.returncode,
                    0,
                    msg=f"{example} failed with {MONAD}\n{result.stdout[-4000:]}",
                )

    def test_syntax_example_links_to_requested_output(self):
        with tempfile.TemporaryDirectory(prefix="monadc-syntax-example-") as td:
            temp = Path(td)
            output = temp / "Syntax"
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            result = subprocess.run(
                [str(MONAD), str(ROOT / "how_to/Syntax.mon"), "-o", str(output)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )

            executable = Path(str(output) + ".exe") if os.name == "nt" else output
            self.assertEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertTrue(executable.exists(), result.stdout[-4000:])

    def test_quickcheck_compiles_and_runs_from_a_read_only_installed_core(self):
        with tempfile.TemporaryDirectory(prefix="monadc-installed-howto-") as td:
            temp = Path(td)
            prefix = temp / "prefix"
            bin_dir = prefix / "bin"
            lib_dir = prefix / "lib"
            installed_core = lib_dir / "monad" / "core"
            work = temp / "work"
            home = temp / "home"
            bin_dir.mkdir(parents=True)
            lib_dir.mkdir(parents=True, exist_ok=True)
            work.mkdir()
            home.mkdir()

            installed_monad = bin_dir / ("monad.exe" if os.name == "nt" else "monad")
            shutil.copy2(MONAD, installed_monad)
            shutil.copytree(ROOT / "core", installed_core)
            shutil.copy2(ROOT / "how_to/QuickCheck.mon", work / "QuickCheck.mon")
            runtime_name = "libmonad.a"
            shutil.copy2(RUNTIME, lib_dir / runtime_name)

            if os.name != "nt":
                for path in installed_core.rglob("*"):
                    path.chmod(0o555 if path.is_dir() else 0o444)
                installed_core.chmod(0o555)

            env = os.environ.copy()
            env["HOME"] = str(home)
            env.pop("MONAD_CORE", None)
            env.pop("MONAD_RUNTIME_LIB", None)
            compile_result = subprocess.run(
                [str(installed_monad), "--test", "QuickCheck.mon"],
                cwd=work,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=30,
            )
            self.assertEqual(
                compile_result.returncode, 0, compile_result.stdout[-4000:]
            )

            executable = (
                work / "QuickCheck.exe" if os.name == "nt"
                else work / "QuickCheck"
            )
            run_result = subprocess.run(
                [str(executable)],
                cwd=work,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=30,
            )
            self.assertEqual(run_result.returncode, 0, run_result.stdout[-4000:])
            self.assertIn("QuickCheck\x1b[0m  Eq Bool", run_result.stdout)
            self.assertIn("QuickCheck\x1b[0m  Additive Int", run_result.stdout)

            law_result = subprocess.run(
                [str(installed_monad), "test", "QuickCheck.mon"],
                cwd=work,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=30,
            )
            self.assertEqual(law_result.returncode, 0, law_result.stdout[-4000:])
            self.assertIn("QuickCheck\x1b[0m  Eq Bool", law_result.stdout)
            self.assertIn("QuickCheck\x1b[0m  Additive Int", law_result.stdout)


if __name__ == "__main__":
    unittest.main()
