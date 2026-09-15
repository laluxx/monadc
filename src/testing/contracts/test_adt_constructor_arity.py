"""Regression coverage for safe ADT-constructor arity diagnostics."""

import os
import pathlib
import subprocess
import tempfile
import unittest


from src.testing.monad_binary import resolve_monad_binary
ROOT = pathlib.Path(__file__).resolve().parents[3]
MONAD = resolve_monad_binary()


class AdtConstructorArityTests(unittest.TestCase):
    def test_missing_constructor_argument_is_diagnosed_without_crashing(self):
        definition = """\
module Result [Result Complete]

data Result
  = Complete Int
"""
        use = """\
import Result

define main :: Int -> Result
  _ -> if True then Complete else Complete 1

main 0
"""
        with tempfile.TemporaryDirectory(prefix="monadc-ctor-arity-") as temp:
            module = pathlib.Path(temp) / "Result.mon"
            program = pathlib.Path(temp) / "Use.mon"
            output = pathlib.Path(temp) / "Missing"
            module.write_text(definition)
            program.write_text(use)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["MONAD_PATH"] = temp
            defined = subprocess.run(
                [str(MONAD), str(module)],
                cwd=temp,
                env=environment,
                capture_output=True,
                text=True,
            )
            self.assertEqual(defined.returncode, 0, defined.stderr)
            compiled = subprocess.run(
                [str(MONAD), str(program), "-o", str(output)],
                cwd=temp,
                env=environment,
                capture_output=True,
                text=True,
            )

        self.assertNotEqual(compiled.returncode, 0)
        self.assertNotEqual(compiled.returncode, -11)
        self.assertIn("constructor 'Complete' requires 1 argument, got 0", compiled.stderr)


if __name__ == "__main__":
    unittest.main()
