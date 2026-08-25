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
define byte :: [1b]

define pointer-id :: *U8 -foreign-> *U8
  value -> asm mov value %rax
               ret

define pointer-byte :: *U8 -> Int -> U8
  bytes index -> (bytes index)

define sys-write :: Int -> *U8 -> Int -foreign-> Int
  descriptor bytes count ->
    asm mov 1          %rax
        mov descriptor %rdi
        mov bytes      %rsi
        mov count      %rdx
        syscall ret

define main :: Int -io-> Int
  _ ->
    set! (byte 0) 65
    sys-write 1 (pointer-id &byte) 1

main 0
"""


class ForeignPointerReturnTests(unittest.TestCase):
    def test_inline_assembly_returns_a_raw_pointer(self):
        with tempfile.TemporaryDirectory(prefix="monadc-pointer-return-") as td:
            temp = Path(td)
            source = temp / "PointerReturn.mon"
            output = temp / "PointerReturn"
            source.write_text(SOURCE)

            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)

            compiled = subprocess.run(
                [str(MONAD), str(source), "-o", str(output)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout[-4000:])

            executed = subprocess.run(
                [str(output)],
                cwd=ROOT,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(executed.returncode, 1, executed.stderr[-4000:])
            self.assertEqual(executed.stdout, b"A")


if __name__ == "__main__":
    unittest.main()
