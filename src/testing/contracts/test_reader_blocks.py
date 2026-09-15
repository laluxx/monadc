import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from src.testing.monad_binary import resolve_monad_binary, resolve_runtime_archive


ROOT = Path(__file__).resolve().parents[3]
MONAD = resolve_monad_binary()
RUNTIME = resolve_runtime_archive(MONAD)


PROVIDER = """\
reader-block decree_ expand-decree

module BlockLanguage [expand-decree]

define expand-decree :: Syntax -> Syntax -> Syntax -> Syntax -> Syntax
  name separator expected-type body -> (syntax-list
                                         (syntax-symbol "define")
                                         (syntax-list name separator expected-type)
                                         (syntax-list-ref (syntax-list-ref body 1) 2))
"""


class ReaderBlockTests(unittest.TestCase):
    def environment(self, root):
        env = os.environ.copy()
        env["HOME"] = str(root / "home")
        Path(env["HOME"]).mkdir(exist_ok=True)
        env["MONAD_CORE"] = str(ROOT / "core")
        env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
        return env

    def compile(self, root, source="Main.mon"):
        return subprocess.run(
            [str(MONAD), source, "-o", str(root / "program")],
            cwd=root,
            env=self.environment(root),
            text=True,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

    def test_direct_import_activates_exported_block_reader(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-block-import-") as td:
            root = Path(td)
            (root / "BlockLanguage.mon").write_text(PROVIDER)
            (root / "Main.mon").write_text(
                "import BlockLanguage\n\n"
                "module Main\n\n"
                "decree answer :: Int\n"
                "  42\n\n"
                "show answer\n"
            )

            compiled = self.compile(root)
            self.assertEqual(compiled.returncode, 0, compiled.stdout[-4000:])
            executed = subprocess.run(
                [str(root / "program")], cwd=root, env=self.environment(root),
                text=True, encoding="utf-8", stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout[-4000:])
            self.assertEqual(executed.stdout, "42\n")

    def test_unimported_block_reader_does_not_change_the_language(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-block-scope-") as td:
            root = Path(td)
            (root / "BlockLanguage.mon").write_text(PROVIDER)
            (root / "Main.mon").write_text(
                "module Main\n\n"
                "decree answer :: Int\n"
                "  42\n\n"
                "show answer\n"
            )

            compiled = self.compile(root)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout[-4000:])
            self.assertNotIn("expanded 'expand-decree'", compiled.stdout)

    def test_two_direct_owners_are_an_explicit_ambiguity(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-block-conflict-") as td:
            root = Path(td)
            (root / "LeftBlocks.mon").write_text(
                PROVIDER.replace("BlockLanguage", "LeftBlocks")
            )
            (root / "RightBlocks.mon").write_text(
                PROVIDER.replace("BlockLanguage", "RightBlocks")
                        .replace("expand-decree", "expand-right")
            )
            (root / "Main.mon").write_text(
                "import LeftBlocks\n"
                "import RightBlocks\n\n"
                "module Main\n\n"
                "decree answer :: Int\n"
                "  42\n"
            )

            compiled = self.compile(root)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout[-4000:])
            self.assertIn("ambiguous reader-block 'decree'", compiled.stdout)
            self.assertIn("LeftBlocks.mon", compiled.stdout)
            self.assertIn("RightBlocks.mon", compiled.stdout)

    def test_invalid_block_pattern_has_a_source_diagnostic(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-block-pattern-") as td:
            root = Path(td)
            (root / "Main.mon").write_text(
                "reader-block _broken expand-broken\n\n"
                "module Main\n"
            )

            compiled = self.compile(root)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout[-4000:])
            self.assertIn("invalid reader-block pattern '_broken'", compiled.stdout)
            self.assertIn("expected: KEYWORD_ TARGET", compiled.stdout)

    def test_configuration_error_points_to_the_literal_entry(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-block-error-") as td:
            root = Path(td)
            source = ROOT / "tests/configuration_literal_invalid_entry.mon"
            result = subprocess.run(
                [str(MONAD), "test", str(source)], cwd=ROOT,
                env=self.environment(root), text=True, encoding="utf-8",
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertIn(
                "configuration_literal_invalid_entry.mon:20:", result.stdout
            )
            self.assertIn(
                "configuration entry must have the form NAME = VALUE",
                result.stdout,
            )


if __name__ == "__main__":
    unittest.main()
