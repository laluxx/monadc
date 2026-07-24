import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()


class CoreStandaloneCompileTests(unittest.TestCase):
    def test_touched_core_modules_compile_with_a_fresh_compiler_home(self):
        modules = (
            "core/prelude/Control/Applicative.mon",
            "core/prelude/Control/Category.mon",
            "core/prelude/Control/Monad.mon",
            "core/prelude/Data/Bool.mon",
            "core/prelude/Data/Either.mon",
            "core/prelude/Data/Eq.mon",
            "core/prelude/Data/Functor.mon",
            "core/prelude/Data/Maybe.mon",
            "core/prelude/Data/Ord.mon",
            "core/prelude/Data/Profunctor.mon",
            "core/prelude/Numeric.mon",
            "core/prelude/Data/Semigroup.mon",
            "core/prelude/Text/Readline.mon",
            "core/prelude/Test/QuickCheck.mon",
        )

        for module in modules:
            with self.subTest(module=module):
                with tempfile.TemporaryDirectory(prefix="monadc-standalone-parent-") as parent:
                    compiler_home = Path(parent) / "new-home"
                    output = Path(parent) / "compiled-module"
                    env = os.environ.copy()
                    env["HOME"] = str(compiler_home)

                    result = subprocess.run(
                        [str(MONAD), module, "-o", str(output)],
                        cwd=ROOT,
                        env=env,
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        check=False,
                    )

                    self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
