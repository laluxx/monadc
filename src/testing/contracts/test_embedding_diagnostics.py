"""Immutable structured compiler diagnostics without stderr parsing."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "src/testing/contracts/fixtures/embedding/branches/diagnostics/atom.embedding.16.diagnostics.immutable-input-check"


class DiagnosticTests(unittest.TestCase):
    def test_immutable_input_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="monadc-diagnostics-") as directory:
            executable = Path(directory) / "host"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "embed/include"), str(FIXTURE / "host.c"),
                 str(ROOT / "libmonad-compiler.a"),
                 str(ROOT / "libmonad-embed.a"), "-pthread", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, (FIXTURE / "stdout").read_text())


if __name__ == "__main__":
    unittest.main()
