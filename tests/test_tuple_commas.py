import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary

ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()


class TupleCommaTests(unittest.TestCase):
    def trace_ast(self, code: str) -> str:
        with tempfile.TemporaryDirectory(prefix="monadc-tuple-") as tmp:
            tmpdir = Path(tmp)
            source = tmpdir / "TupleComma.mon"
            source.write_text(f"{code}\n", encoding="utf-8")
            home = tmpdir / "home"
            core = home / "core"
            home.mkdir()
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(core)

            proc = subprocess.run(
                [str(MONAD), "--trace=ast", str(source)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertIn("=== desugared AST", proc.stderr)
            self.assertIn("=== end desugared AST ===", proc.stderr)
            return proc.stdout.strip()

    def test_comma_tuple_literal_survives_as_tuple_value(self):
        self.assertEqual(self.trace_ast('(show (1, "x"))'), '(show (1 , "x"))')

    def test_comma_tuple_supports_three_or_more_fields(self):
        self.assertEqual(self.trace_ast("(show (1, 2, 3))"), "(show (1 , 2 , 3))")


if __name__ == "__main__":
    unittest.main()
