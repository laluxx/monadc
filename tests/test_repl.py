import os
import re
import subprocess
import unittest

from monad_binary import resolve_monad_binary


MONAD = resolve_monad_binary()


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def clean_output(value: str) -> str:
    return ANSI_RE.sub("", value).strip()


class ReplTests(unittest.TestCase):
    def run_repl(self, source: str) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env["MONAD_NO_PROMPT"] = "1"
        return subprocess.run(
            [str(MONAD), "repl"],
            input=source,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=15,
        )

    def test_repl_evaluates_piped_one_line_wisp(self):
        result = self.run_repl("show 42\n")

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(clean_output(result.stdout), "42")

    def test_repl_bare_integer_expression_prints_one_line(self):
        result = self.run_repl("3 + 3\n")

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(clean_output(result.stdout), "6")

    def test_repl_persists_top_level_value_definitions(self):
        result = self.run_repl(
            "define x 30\n"
            "x\n"
            "show (x + 12)\n"
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        lines = clean_output(result.stdout).splitlines()
        self.assertNotIn("runtime crash", result.stdout)
        self.assertNotIn("JIT session error", result.stdout)
        self.assertNotIn("ORC lookup failed", result.stdout)
        self.assertEqual(lines[0], "30")
        self.assertEqual(lines[-1], "42")

    def test_repl_evaluates_multiline_wisp_typed_function_then_call(self):
        result = self.run_repl(
            "define add2 :: Int -> Int\n"
            "  x -> x + 2\n"
            "\n"
            "show (add2 40)\n"
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("42", clean_output(result.stdout).splitlines())

    def test_repl_evaluates_multiline_wisp_pattern_clauses(self):
        result = self.run_repl(
            "define choose :: Int -> Int\n"
            "  0 -> 10\n"
            "  x -> x + 1\n"
            "\n"
            "show (choose 0)\n"
            "show (choose 4)\n"
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(clean_output(result.stdout).splitlines(), ["10", "5"])


if __name__ == "__main__":
    unittest.main()
