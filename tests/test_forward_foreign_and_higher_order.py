import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary, resolve_runtime_archive


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()
RUNTIME = resolve_runtime_archive(MONAD)


class ForwardForeignAndHigherOrderTests(unittest.TestCase):
    def compile_and_run(self, source_text):
        with tempfile.TemporaryDirectory(prefix="monadc-forward-foreign-") as td:
            temp = Path(td)
            source = temp / "Main.mon"
            output = temp / "Main"
            source.write_text(source_text, encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            compiled = subprocess.run(
                [str(MONAD), str(source), "-o", str(output)],
                cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout[-5000:])
            run = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=10,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-5000:])
            return run.stdout

    def test_wrapper_calls_later_defined_foreign_function_directly(self):
        output = self.compile_and_run(
            "define call-later :: Int -> Int\n"
            "  value -> foreign-increment value\n\n"
            "define foreign-increment :: Int -foreign-> Int\n"
            "  value ->\n"
            "    asm mov 42 %rax\n"
            "        ret\n\n"
            "show (call-later 41)\n"
        )
        self.assertEqual(output, "42\n")

    def test_recursive_higher_order_io_function_keeps_handler_as_one_argument(self):
        output = self.compile_and_run(
            "define apply-handler :: (Int -> Int) -> Int -io-> Int\n"
            "  handler value -> handler value\n\n"
            "define apply-many :: Int -> (Int -> Int) -> Int -io-> Int\n"
            "  count _ value | (<= count 0) -> value\n"
            "  count handler value ->\n"
            "    apply-many (count - 1) handler (apply-handler handler value)\n\n"
            "define increment :: Int -> Int\n"
            "  value -> value + 1\n\n"
            "show (apply-many 3 increment 39)\n"
        )
        self.assertEqual(output, "42\n")


if __name__ == "__main__":
    unittest.main()
