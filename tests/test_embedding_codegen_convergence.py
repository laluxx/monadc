import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/embedding/branches/codegen-convergence/atom.embedding.43.codegen-convergence.production-conditional"


class EmbeddingCodegenConvergenceTest(unittest.TestCase):
    def test_runtime_source_uses_production_expression_semantics(self):
        with tempfile.TemporaryDirectory(prefix="monadc-codegen-convergence-") as directory:
            executable = pathlib.Path(directory) / "host"
            subprocess.run(
                [
                    "cc", "-std=c99", "-I", str(ROOT / "embed/include"),
                    str(FIXTURE / "host.c"), "-L", str(ROOT),
                    "-Wl,-rpath," + str(ROOT), "-lmonad-compiler", "-lmonad-embed",
                    "-o", str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            completed = subprocess.run(
                [str(executable)], check=True, text=True, capture_output=True,
                env={**os.environ, "LD_LIBRARY_PATH": str(ROOT)},
            )
            self.assertEqual(completed.stdout, (FIXTURE / "stdout").read_text())


if __name__ == "__main__":
    unittest.main()
