import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary


MONAD = resolve_monad_binary()
ROOT = Path(__file__).resolve().parents[1]


class TailCallTests(unittest.TestCase):
    def run_monad(self, args):
        env = os.environ.copy()
        env["MONAD_CORE"] = str(ROOT / "core")
        return subprocess.run(
            [str(MONAD), *args],
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=30,
        )

    def test_closure_abi_tail_dispatch_uses_musttail(self):
        source = """\
(module Main)
(define make-apply (lambda ([bias : Int] -> Fn) (lambda ([f : Fn] [x : Int] -> Int) (f (+ x bias)))))
(define inc (lambda ([x : Int] -> Int) (+ x 1)))
(define apply2 (make-apply 1))
(show (apply2 inc 40))
"""
        with tempfile.TemporaryDirectory(prefix="monadc-musttail-") as tmp:
            src = Path(tmp) / "musttail.mon"
            out = Path(tmp) / "musttail"
            src.write_text(source)

            compile_result = self.run_monad([str(src), "--emit-ir", "-o", str(out)])
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)

            run_result = subprocess.run(
                [str(out)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=10,
            )
            self.assertEqual(run_result.returncode, 0, run_result.stdout)
            self.assertIn("42", run_result.stdout.splitlines())

            ir = src.with_suffix(".ll").read_text()
            self.assertRegex(
                ir,
                re.compile(
                    r"define ptr @__anon_[^{]+\(ptr %env, i32 %[^,]+, ptr %[^)]+\)"
                    r"\s*\{(?:(?!^}).)*musttail call ptr %tail_clo_fn"
                    r"\(ptr %tail_clo_env, i32 1, ptr %[^)]+\)\s+ret ptr",
                    re.MULTILINE | re.DOTALL,
                ),
            )


if __name__ == "__main__":
    unittest.main()
