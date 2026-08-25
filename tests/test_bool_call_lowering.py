import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary, resolve_runtime_archive


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()
RUNTIME = resolve_runtime_archive(MONAD)


SOURCE = """\
define truth-name :: Bool -> String
  value | value     -> "true"
        | otherwise -> "false"

show truth-name True
show truth-name False
"""


class BoolCallLoweringTests(unittest.TestCase):
    def test_typed_calls_preserve_native_boolean_tags(self):
        with tempfile.TemporaryDirectory(prefix="monadc-bool-call-") as td:
            temp = Path(td)
            source = temp / "BoolCall.mon"
            output = temp / "BoolCall"
            source.write_text(SOURCE, encoding="utf-8")

            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)

            compiled = subprocess.run(
                [str(MONAD), str(source), "-o", str(output)],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout[-4000:])

            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout[-4000:])
            self.assertEqual(executed.stdout, "true\nfalse\n")


if __name__ == "__main__":
    unittest.main()
