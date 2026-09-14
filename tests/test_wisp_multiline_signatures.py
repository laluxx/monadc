#!/usr/bin/env python3
"""Regression coverage for layout-continuation lines in Wisp signatures."""

import os
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MONAD = ROOT / "monad"


class WispMultilineSignatureTests(unittest.TestCase):
    def compile_and_run(self, source: str):
        with tempfile.TemporaryDirectory(prefix="monad-wisp-signature-") as tmp:
            work = Path(tmp)
            program = work / "Main.mon"
            output = work / "Main"
            core = work / "core"
            core.mkdir()
            program.write_text(textwrap.dedent(source).lstrip(), encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(work / "home")
            env["MONAD_CORE"] = str(core)
            result = subprocess.run(
                [str(MONAD), str(program), "-o", str(output)],
                cwd=work,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            runtime = None
            if result.returncode == 0:
                runtime = subprocess.run(
                    [str(output)], cwd=work, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True,
                )
            return result, runtime

    def test_signature_may_start_on_indented_next_line(self):
        result, runtime = self.compile_and_run("""
            module Main
            define identity ::
              Int -> Int
              value -> value
            show (identity 42)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIsNotNone(runtime)
        self.assertEqual(runtime.returncode, 0, runtime.stdout)
        self.assertEqual(runtime.stdout, "42\n")

    def test_constrained_signature_may_span_multiple_continuation_lines(self):
        result, runtime = self.compile_and_run("""
            module Main
            class Marker a where
              mark :: a -> a
            instance Marker Int
              mark value -> value
            define select :: Marker a =>
              (a -> a) -> a
              -> a
              function value -> function value
            show (select (lambda (value) value) 42)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIsNotNone(runtime)
        self.assertEqual(runtime.returncode, 0, runtime.stdout)
        self.assertEqual(runtime.stdout, "42\n")

    def test_multiline_lambda_remains_one_call_argument(self):
        result, runtime = self.compile_and_run("""
            module Main
            define apply-one :: (Int -> Int) -> Int -> Int
              function value -> function value
            define increment-through :: Int -> Int
              value -> apply-one
                (lambda (item)
                  item + 1)
                value
            show (increment-through 41)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIsNotNone(runtime)
        self.assertEqual(runtime.returncode, 0, runtime.stdout)
        self.assertEqual(runtime.stdout, "42\n")

    def test_multiline_define_accepts_doc_metadata_before_clause(self):
        result, runtime = self.compile_and_run("""
            module Main
            define cell-at :: [Int] -> Int -> Int
              cells index -> cells index
            define rule-101 :: Int -> Int -> Int -> Int
              left center right -> left + center + right
            define next-cell :: [Int] -> Int -> Int
              :doc "Return the Rule 101 value for the cell at INDEX in the next generation."
              cells index ->
                rule-101 (cell-at cells (index - 1)) (cell-at cells index) (cell-at cells (index + 1))
            show (next-cell [10 20 12] 1)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIsNotNone(runtime)
        self.assertEqual(runtime.returncode, 0, runtime.stdout)
        self.assertEqual(runtime.stdout, "3\n")

    def test_parenthesized_multiline_match_is_not_wrapped_twice(self):
        result, runtime = self.compile_and_run("""
            module Main
            define choice-value :: Int -> Int
              value -> (match value with
                | 42 -> value
                | _ -> 0)
            show (choice-value 42)
            show (choice-value 7)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIsNotNone(runtime)
        self.assertEqual(runtime.returncode, 0, runtime.stdout)
        self.assertEqual(runtime.stdout, "42\n0\n")

    def test_parenthesized_optional_return_is_not_an_extra_parameter(self):
        result, runtime = self.compile_and_run("""
            module Main
            define maybe-pair :: Int -> (Int, Int)?
              _ -> nil
            show (maybe-pair 42)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIsNotNone(runtime)
        self.assertEqual(runtime.returncode, 0, runtime.stdout)
        self.assertEqual(runtime.stdout, "nil\n")


if __name__ == "__main__":
    unittest.main()
