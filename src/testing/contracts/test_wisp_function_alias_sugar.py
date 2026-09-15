#!/usr/bin/env python3
"""Regression coverage for aliases declared in a Wisp define header."""

import os
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


from src.testing.monad_binary import resolve_monad_binary
ROOT = Path(__file__).resolve().parents[3]
MONAD = resolve_monad_binary()


class WispFunctionAliasSugarTests(unittest.TestCase):
    def test_every_name_before_signature_separator_is_a_callable_alias(self):
        source = """
            module Main
            define increment inc plus-one ∪ :: Int -> Int
              value -> value + 1
            show (increment 1)
            show (inc 2)
            show (plus-one 3)
            show (∪ 4)
        """
        with tempfile.TemporaryDirectory(prefix="monad-wisp-alias-") as tmp:
            work = Path(tmp)
            program = work / "Main.mon"
            output = work / "Main"
            core = work / "core"
            core.mkdir()
            program.write_text(textwrap.dedent(source).lstrip(), encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(work / "home")
            env["MONAD_CORE"] = str(core)
            compiled = subprocess.run(
                [str(MONAD), str(program), "-o", str(output)],
                cwd=work, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            runtime = subprocess.run(
                [str(output)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(runtime.returncode, 0, runtime.stdout)
            self.assertEqual(runtime.stdout, "2\n3\n4\n5\n")

    def test_constrained_header_accepts_multiple_aliases(self):
        source = """
            import Data.Eq
            module Main
            define same? equivalent? hello wow :: Eq a => a -> a -> Bool
              left right -> (= left right)
            show (same? 2 2)
            show (equivalent? 2 2)
            show (hello 2 2)
            show (wow 2 2)
        """
        with tempfile.TemporaryDirectory(prefix="monad-wisp-alias-import-") as tmp:
            work = Path(tmp)
            program = work / "Main.mon"
            output = work / "Main"
            program.write_text(textwrap.dedent(source).lstrip(), encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(work / "home")
            env["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(MONAD), str(program), "-o", str(output)],
                cwd=work, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            runtime = subprocess.run(
                [str(output)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(runtime.returncode, 0, runtime.stdout)
            self.assertEqual(runtime.stdout, "True\nTrue\nTrue\nTrue\n")


if __name__ == "__main__":
    unittest.main()
