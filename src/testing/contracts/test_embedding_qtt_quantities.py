"""Installed reports own exact production-solved QTT quantities."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "src/testing/contracts/fixtures/embedding/branches/introspection/atom.embedding.35.introspection.solved-quantities"


class QttQuantityTests(unittest.TestCase):
    def test_installed_report_distinguishes_used_and_erased_parameters(self):
        with tempfile.TemporaryDirectory(prefix="monadc-qtt-quantity-") as directory:
            prefix = Path(directory) / "sdk"
            installed = subprocess.run(
                ["make", "install", f"PREFIX={prefix}"], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(installed.returncode, 0, installed.stdout)
            executable = Path(directory) / "host"
            built = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(prefix / "include"), str(FIXTURE / "host.c"),
                 str(prefix / "lib/libmonad-compiler.a"),
                 str(prefix / "lib/libmonad-embed.a"), "-pthread", "-lm",
                 "-o", str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(built.returncode, 0, built.stdout)
            ran = subprocess.run([str(executable)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, (FIXTURE / "stdout").read_text())


if __name__ == "__main__":
    unittest.main()
