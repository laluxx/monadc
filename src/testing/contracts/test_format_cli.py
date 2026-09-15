import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from src.testing.monad_binary import resolve_monad_binary


ROOT = Path(__file__).resolve().parents[3]
MONAD = resolve_monad_binary()


class FormatCliTests(unittest.TestCase):
    def invoke(self, *args):
        return subprocess.run([str(MONAD), *args], cwd=ROOT, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              check=False)

    def test_command_local_help_works_in_suffix_position(self):
        formatter = self.invoke("format", "help")
        self.assertEqual(formatter.returncode, 0, formatter.stderr)
        self.assertIn("monad format", formatter.stderr)
        self.assertIn("--control-flow=glyph", formatter.stderr)
        self.assertIn("--control-flow=ascii", formatter.stderr)
        self.assertIn("--docstrings=glyph", formatter.stderr)
        self.assertIn("--docstrings=inline", formatter.stderr)
        build = self.invoke("build", "help")
        self.assertEqual(build.returncode, 0, build.stderr)
        self.assertIn("monad build", build.stderr)
        self.assertNotIn("monad format", build.stderr)
        for command in ("new", "run", "clean", "install", "test", "check",
                        "lint", "eval", "repl", "jit", "debug", "lsp", "spirv"):
            with self.subTest(command=command):
                page = self.invoke(command, "help")
                self.assertEqual(page.returncode, 0, page.stderr)
                self.assertIn(f"monad {command}", page.stderr)

    def test_menu_is_removed_from_cli_and_help(self):
        top = self.invoke("help")
        self.assertEqual(top.returncode, 0)
        self.assertNotIn("interactive browser", top.stderr.lower())
        self.assertNotRegex(top.stderr, r"(?m)^\s+menu(?:,|\s)")
        menu = self.invoke("menu")
        self.assertNotEqual(menu.returncode, 0)
        self.assertIn("unknown command", menu.stderr.lower())

    def test_control_flow_styles_are_idempotent_and_round_trip_to_canonical_ascii(self):
        ascii_source = '''define grade :: Int -> String
  score | < 60 -> "F"
        | < 70 -> "D"
        | < 80 -> "C"
        | < 90 -> "B"
        | otherwise -> "A"
'''
        with tempfile.TemporaryDirectory(prefix="monadc-format-") as td:
            path = Path(td) / "Grade.mon"
            path.write_text(ascii_source, encoding="utf-8")
            glyph = self.invoke("format", "--control-flow=glyph", str(path))
            self.assertEqual(glyph.returncode, 0, glyph.stderr)
            self.assertIn("score ├─ < 60 ->", glyph.stdout)
            self.assertIn("╰───▶ \"A\"", glyph.stdout)
            path.write_text(glyph.stdout, encoding="utf-8")
            glyph2 = self.invoke("format", "--control-flow=glyph", str(path))
            self.assertEqual(glyph2.stdout, glyph.stdout)
            ascii_result = self.invoke("format", "--control-flow=ascii", str(path))
            self.assertEqual(ascii_result.returncode, 0, ascii_result.stderr)
            path.write_text(ascii_result.stdout, encoding="utf-8")
            ascii2 = self.invoke("format", "--control-flow=ascii", str(path))
            self.assertEqual(ascii2.stdout, ascii_result.stdout)
            glyph3 = self.invoke("format", "--control-flow=glyph", str(path))
            self.assertEqual(glyph3.stdout, glyph.stdout)

    def test_docstring_styles_round_trip_with_presentation_punctuation(self):
        inline_source = '''method not? :: Bool -> Bool
  x | x       -> False
    | otherwise -> True
    :doc "Boolean negation."
'''
        with tempfile.TemporaryDirectory(prefix="monadc-format-doc-") as td:
            path = Path(td) / "Bool.mon"
            path.write_text(inline_source, encoding="utf-8")
            glyph = self.invoke("format", "--control-flow=ascii", "--docstrings=glyph", str(path))
            self.assertEqual(glyph.returncode, 0, glyph.stderr)
            self.assertIn("╭─ Boolean negation\nmethod not?", glyph.stdout)
            self.assertNotIn("╭─ Boolean negation.\n", glyph.stdout)
            self.assertNotIn(":doc", glyph.stdout)

            path.write_text(glyph.stdout, encoding="utf-8")
            inline = self.invoke("format", "--control-flow=ascii", "--docstrings=inline", str(path))
            self.assertEqual(inline.returncode, 0, inline.stderr)
            self.assertIn('    :doc "Boolean negation."', inline.stdout)
            self.assertNotIn("╭─", inline.stdout)

            path.write_text(inline.stdout, encoding="utf-8")
            inline2 = self.invoke("format", "--control-flow=ascii", "--docstrings=inline", str(path))
            self.assertEqual(inline2.stdout, inline.stdout)


if __name__ == "__main__":
    unittest.main()
