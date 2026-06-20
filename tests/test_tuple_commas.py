import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MONAD = ROOT / "monad"


class TupleCommaTests(unittest.TestCase):
    def run_eval(self, code: str) -> str:
        proc = subprocess.run(
            [str(MONAD), "eval", code],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(
            proc.returncode,
            0,
            msg=f"monad eval failed\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}",
        )
        return proc.stdout.strip()

    def test_comma_tuple_literal_survives_as_tuple_value(self):
        self.assertEqual(self.run_eval('(show (1, "x"))'), '(1 "x")')

    def test_comma_tuple_supports_three_or_more_fields(self):
        self.assertEqual(self.run_eval("(show (1, 2, 3))"), "(1 2 3)")


if __name__ == "__main__":
    unittest.main()
