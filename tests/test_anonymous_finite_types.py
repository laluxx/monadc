import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import generated_executable, resolve_monad_binary


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()


class AnonymousFiniteTypeTests(unittest.TestCase):
    def test_signature_set_is_a_structural_compact_type(self):
        source = """\
(module Main)
define choose :: Bool -> {Yes No}
  x -> Yes

define keep :: {Yes, No} -> {Yes No}
  x -> x

(show (keep (choose True)))
"""
        with tempfile.TemporaryDirectory(prefix="monadc-anon-finite-") as td:
            temp = Path(td)
            home = temp / "home"
            src = temp / "Main.mon"
            out = temp / "main"
            home.mkdir()
            src.write_text(source, encoding="utf-8")

            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(ROOT / "core")
            result = subprocess.run(
                [str(MONAD), str(src), "-O", "--emit-ir", "-o", str(out)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout)

            execution = subprocess.run(
                [str(generated_executable(out))],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=10,
            )
            self.assertEqual(execution.returncode, 0, execution.stdout)
            self.assertEqual(execution.stdout.strip(), "Yes")

            ir = src.with_suffix(".ll").read_text(encoding="utf-8")
            self.assertRegex(ir, r"define\s+i1\s+@(?:choose|\"choose\")\(i1")
            self.assertRegex(ir, r"define\s+i1\s+@(?:keep|\"keep\")\(i1")


if __name__ == "__main__":
    unittest.main()
