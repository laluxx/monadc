import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary, resolve_runtime_archive


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()
RUNTIME = resolve_runtime_archive(MONAD)


class HowToExampleTests(unittest.TestCase):
    EXAMPLES = (
        "how_to/Syntax.mon",
        "how_to/AlgebraicDataTypes.mon",
        "how_to/Macros.mon",
        "how_to/Iter.mon",
    )

    def test_readme_listed_how_to_examples_compile(self):
        for example in self.EXAMPLES:
            with self.subTest(example=example):
                with tempfile.TemporaryDirectory(prefix="monadc-howto-home-") as home:
                    env = os.environ.copy()
                    env["HOME"] = home
                    env["MONAD_CORE"] = str(ROOT / "core")
                    env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
                    result = subprocess.run(
                        [str(MONAD), str(ROOT / example)],
                        cwd=ROOT,
                        env=env,
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        check=False,
                    )

                self.assertEqual(
                    result.returncode,
                    0,
                    msg=f"{example} failed with {MONAD}\n{result.stdout[-4000:]}",
                )


if __name__ == "__main__":
    unittest.main()
