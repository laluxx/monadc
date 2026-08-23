import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()


class WispGroupedInfixTests(unittest.TestCase):
    def compile_source(self, source_text: str):
        tmp = tempfile.TemporaryDirectory(prefix="monadc-wisp-infix-")
        tmpdir = Path(tmp.name)
        source = tmpdir / "Main.mon"
        source.write_text(source_text, encoding="utf-8")
        env = os.environ.copy()
        env["HOME"] = str(tmpdir / "home")
        env["MONAD_CORE"] = str(ROOT / "core")
        result = subprocess.run(
            [str(MONAD), "--trace=ast", str(source)],
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        tmp.cleanup()
        return result

    def test_data_declaration_equals_is_not_promoted_to_an_expression(self):
        source_text = """\
module Main

data Maybe a
  = Nothing
  | Just a
"""
        with tempfile.TemporaryDirectory(prefix="monadc-wisp-data-") as tmp:
            tmpdir = Path(tmp)
            source = tmpdir / "Main.mon"
            source.write_text(source_text, encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(tmpdir / "home")
            env["MONAD_CORE"] = str(ROOT / "core")

            result = subprocess.run(
                [str(MONAD), "--trace=ast", str(source)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("(data Maybe Nothing | Just a)", result.stdout)
        self.assertNotIn("(= (data Maybe", result.stdout)

    def test_unguarded_concat_chain_lowers_every_operator(self):
        result = self.compile_source("""\
module Main
define render :: String -> String
  value -> "(" ++ value ++ ")"
""")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('(append "(" (append value ")"))', result.stdout)

    def test_space_string_pattern_body_stays_a_string_literal(self):
        result = self.compile_source('''\
module Main
define cell-char :: Int -> String
  0 -> " "
  _ -> "#"

show (cell-char 0) ++ (cell-char 1)
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('(if (= __p_0 0) " " "#")', result.stdout)
        self.assertNotIn('(" ")', result.stdout)

    def test_subject_only_guarded_method_keeps_its_parameters(self):
        result = self.compile_source("""\
module Main
method choose :: Int -> Int -> Int -> Int
  start step end
    | step > 0 -> start
    | step < 0 -> end
  start _ _ -> 0
""")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("(define Main.choose", result.stdout)

    def test_subject_only_guarded_class_default_builds_one_method_body(self):
        result = self.compile_source("""\
module Main
class Choose a where
  choose :: a -> a -> a
  choose left right
    | left > right -> left
    | otherwise -> right

instance Choose Int
""")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("(class Choose a where", result.stdout)


if __name__ == "__main__":
    unittest.main()
