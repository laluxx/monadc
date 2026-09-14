import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()


class CoreStandaloneCompileTests(unittest.TestCase):
    def test_all_core_modules_compile_with_a_fresh_compiler_home(self):
        modules = tuple(
            str(path.relative_to(ROOT))
            for path in sorted((ROOT / "core").rglob("*.mon"))
        )

        self.assertEqual(
            len(modules),
            77,
            "the standardized verified Core baseline contains 77 modules",
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
