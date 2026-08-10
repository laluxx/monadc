"""Compiler-owned Monad source linting and recursive project discovery."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MONAD = ROOT / "monad"


class MonadLintTests(unittest.TestCase):
    def run_lint(self, *arguments):
        return subprocess.run(
            [str(MONAD), "lint", *map(str, arguments)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    def test_clean_file_exits_successfully(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "Clean.mon"
            source.write_text(
                "define small? :: Int -> Bool\n"
                "  n | < 2       -> True\n"
                "    | otherwise -> False\n"
            )
            result = self.run_lint(source)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("no lint diagnostics", result.stdout)

    def test_large_flat_module_exports_suggest_grouped_catalogue(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "Large.mon"
            exports = " ".join(f"value-{index}" for index in range(600))
            source.write_text(f"module Large\n  [{exports}]\n")
            result = self.run_lint("--json", source)
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn('"rule":"style/grouped-module-exports"', result.stdout)
            self.assertIn("module … where", result.stdout)

    def test_diagnostic_has_rule_span_and_suggested_rewrite(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "Guard.mon"
            source.write_text(
                "define small? :: Int -> Bool\n"
                "  n | n < 2     -> True\n"
                "    | otherwise -> False\n"
            )
            result = self.run_lint(source)
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn(f"{source}:2:7: warning", result.stdout)
            self.assertIn("[style/unary-receiver]", result.stdout)
            self.assertIn("write `| < 2`", result.stdout)

    def test_directory_lint_is_recursive_and_ignores_hidden_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nested = root / "src" / "nested"
            nested.mkdir(parents=True)
            (nested / "Bad.mon").write_text(
                "define f :: Int -> Int\n  n | n = 0 -> 0\n    | otherwise -> n\n"
            )
            hidden = root / ".cache"
            hidden.mkdir()
            (hidden / "Ignored.mon").write_text(
                "define g :: Int -> Int\n  n | n = 0 -> 0\n    | otherwise -> n\n"
            )
            result = self.run_lint(root)
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn("Bad.mon:2", result.stdout)
            self.assertNotIn("Ignored.mon", result.stdout)
            self.assertIn("1 diagnostic in 1 file", result.stdout)

    def test_json_output_is_stable_for_tooling(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "Asm.mon"
            source.write_text(
                "define raw :: Int -foreign-> Int\n"
                "  fd -> (asm mov fd %rax ret)\n"
            )
            result = self.run_lint("--json", source)
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn('"rule":"style/multiline-asm"', result.stdout)
            self.assertIn('"line":2', result.stdout)
            self.assertIn('"severity":"warning"', result.stdout)
            self.assertIn('"phase":"source"', result.stdout)
            self.assertIn('"fixSafety":"suggested"', result.stdout)
            self.assertIn('"origin":"compiler"', result.stdout)
            self.assertIn('"complete":true', result.stdout)
            self.assertIn('"applicability":"maybe-incorrect"', result.stdout)

    def test_source_rules_do_not_lint_strings_or_ordinary_comments(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "Quoted.mon"
            source.write_text(
                'define example :: String\n'
                '  ";; prose containing (asm mov x %rax)"\n'
                ';; (asm mov x %rax)\n'
            )
            result = self.run_lint(source)
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_fix_applies_only_a_semantics_preserving_replacement(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "Fix.mon"
            source.write_text(
                "define small? :: Int -> Bool\n"
                "  n | n < 2     -> True\n"
                "    | otherwise -> False\n"
            )
            fixed = self.run_lint("--fix", source)
            self.assertEqual(fixed.returncode, 0, fixed.stdout)
            self.assertEqual(
                source.read_text(),
                "define small? :: Int -> Bool\n"
                "  n | < 2     -> True\n"
                "    | otherwise -> False\n",
            )
            checked = self.run_lint(source)
            self.assertEqual(checked.returncode, 0, checked.stdout)

    def test_safe_fix_is_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "Idempotent.mon"
            source.write_text(
                "define small? :: Int -> Bool\n"
                "  n | n < 2 -> True\n"
                "    | otherwise -> False\n"
            )
            first = self.run_lint("--fix", source)
            self.assertEqual(first.returncode, 0, first.stdout)
            normalized = source.read_bytes()
            second = self.run_lint("--fix", source)
            self.assertEqual(second.returncode, 0, second.stdout)
            self.assertEqual(source.read_bytes(), normalized)
            self.assertNotIn("fixed", second.stdout)

    def test_fix_normalizes_proven_unary_guard_calls(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "Word.mon"
            source.write_text(
                "define word-char? :: Int -> Bool\n"
                "  c | (and (>= c 0x30) (<= c 0x39)) -> True\n"
                "  c | (= c 0x5f)                    -> True\n"
                "  c                                 -> False\n"
            )
            diagnosed = self.run_lint("--json", source)
            self.assertEqual(diagnosed.returncode, 1, diagnosed.stdout)
            self.assertIn('"rule":"style/receiver-relative-guard"', diagnosed.stdout)
            self.assertIn('"applicability":"machine-applicable"', diagnosed.stdout)

            fixed = self.run_lint("--fix", source)
            self.assertEqual(fixed.returncode, 0, fixed.stdout)
            self.assertEqual(
                source.read_text(),
                "define word-char? :: Int -> Bool\n"
                "  c | >= 0x30 and <= 0x39 -> True\n"
                "    | = 0x5f                    -> True\n"
                "    | otherwise -> False\n",
            )
            checked = self.run_lint(source)
            self.assertEqual(checked.returncode, 0, checked.stdout)

    def test_json_exposes_shared_structured_edits(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "Edit.mon"
            source.write_text(
                "define small? :: Int -> Bool\n"
                "  n | n < 2 -> True\n"
                "    | otherwise -> False\n"
            )
            result = self.run_lint("--json", source)
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn('"edits":[{', result.stdout)
            self.assertIn('"applicability":"machine-applicable"', result.stdout)
            self.assertIn('"replacement":""', result.stdout)
        self.assertIn('"related":[]', result.stdout)

    def test_commentary_prose_drops_redundant_comment_disguise(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "Readline.mon"
            source.write_text(
                ";;; Readline.mon --- terminal input\n"
                "\n"
                ";;; Commentary:\n"
                "\n"
                ";; IO.Readline owns terminal interaction.\n"
                ";;\n"
                ";; The terminal handle remains scoped.\n"
                "\n"
                ";;; Code:\n"
                "\n"
                "module IO.Readline []\n"
            )
            diagnosed = self.run_lint("--json", source)
            self.assertEqual(diagnosed.returncode, 1, diagnosed.stdout)
            self.assertIn('"rule":"style/commentary-prose"', diagnosed.stdout)
            self.assertIn("a little sussy", diagnosed.stdout)

            fixed = self.run_lint("--fix", source)
            self.assertEqual(fixed.returncode, 0, fixed.stdout)
            self.assertEqual(
                source.read_text(),
                ";;; Readline.mon --- terminal input\n"
                "\n"
                ";;; Commentary:\n"
                "\n"
                " IO.Readline owns terminal interaction.\n"
                "\n"
                " The terminal handle remains scoped.\n"
                "\n"
                ";;; Code:\n"
                "\n"
                "module IO.Readline []\n",
            )
            self.assertEqual(self.run_lint(source).returncode, 0)

    def test_recursive_core_lint_is_clean(self):
        result = self.run_lint(ROOT / "core")
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
