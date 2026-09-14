import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary


ROOT = Path(__file__).resolve().parents[1]
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


if __name__ == "__main__":
    unittest.main()
