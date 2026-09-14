import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()


class WispGlyphControlFlowTests(unittest.TestCase):
    def compile_and_run(self, source_text: str):
        with tempfile.TemporaryDirectory(prefix="monadc-glyph-flow-") as td:
            tmp = Path(td)
            source = tmp / "Main.mon"
            output = tmp / "Main"
            source.write_text(source_text, encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(tmp / "home")
            env["MONAD_CORE"] = str(ROOT / "core")
            built = subprocess.run(
                [str(MONAD), str(source), "-o", str(output)], cwd=ROOT,
                env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False,
            )
            if built.returncode != 0:
                return built, None
            ran = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            )
            return built, ran

    def test_canonical_glyph_trees_cover_inline_dropped_and_nested_forms(self):
        built, ran = self.compile_and_run('''\
module Main

define fib :: Int -> Int
  n ├─ < 0 -> 99
    ├─ = 0 -> 0
    ├─ = 1 -> 1
    ╰─────▶ 21

define pow-int :: Int -> Int -> Int
  base exp ─╮
            ├─ exp < 0 -> 99
            ├─ exp = 0 -> 1
            ╰─────────▶ 32

define numsign :: Int -> String
  n ├─ < 0 -> "negative"
    ╰─╮
      ├─ = 0 -> "zero"
      ╰───▶ "positive"

define grade :: Int -> String
  score ├─ < 60 -> "F"
        ╰─╮
          ├─ < 70 -> "D"
          ├─ < 80 -> "C"
          ├─ < 90 -> "B"
          ╰───▶ "A"

show (fib 8)
show (pow-int 2 5)
show (numsign 0)
show (grade 83)
''')
        self.assertEqual(built.returncode, 0, built.stderr)
        self.assertIsNotNone(ran)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertEqual(ran.stdout.splitlines(), ["21", "32", "zero", "B"])

    def test_noncanonical_glyph_leaf_is_rejected(self):
        built, _ = self.compile_and_run('''\
define bad :: Int -> Int
  n ├─ < 0 ▶ 0
    ╰──▶ 1
''')
        self.assertNotEqual(built.returncode, 0)
        self.assertIn("glyph control flow", built.stderr)

    def test_glyphs_in_strings_and_comments_remain_data(self):
        built, ran = self.compile_and_run('''\
module Main
; ├─ and ▶ are prose here
show "├─ ╰─╮ ▶"
''')
        self.assertEqual(built.returncode, 0, built.stderr)
        self.assertIsNotNone(ran)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertEqual(ran.stdout.strip(), "├─ ╰─╮ ▶")


if __name__ == "__main__":
    unittest.main()
