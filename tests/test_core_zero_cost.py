import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import generated_executable, resolve_monad_binary


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()


class CoreZeroCostTests(unittest.TestCase):
    def test_core_bool_method_specializes_without_runtime_dispatch(self):
        source = """\
(module Main)
(import Data.Bool)
(define (invert? [x : Bool] -> Bool) (not? x))
(show (invert? True))
"""
        with tempfile.TemporaryDirectory(prefix="monadc-core-zero-cost-") as td:
            temp = Path(td)
            home = temp / "home"
            src = temp / "bool-specialize.mon"
            out = temp / "bool-specialize"
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
            self.assertEqual(execution.stdout.strip(), "False")

            ir = src.with_suffix(".ll").read_text(encoding="utf-8")
            invert = re.search(
                r'define\s+i1\s+@"invert\?"\([^)]*\)\s*\{(?P<body>.*?)^}',
                ir,
                re.MULTILINE | re.DOTALL,
            )
            self.assertIsNotNone(invert, ir)
            body = invert.group("body")
            self.assertNotRegex(body, r"\bcall\b", body)
            self.assertRegex(body, r"(?:xor|icmp)", body)


if __name__ == "__main__":
    unittest.main()
