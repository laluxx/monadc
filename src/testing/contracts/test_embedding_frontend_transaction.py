"""Single-entry reader/Wisp transaction recovery regression."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "src/testing/contracts/fixtures/embedding/branches/frontend-transaction/atom.embedding.21.frontend-transaction.wisp-recovery"


class FrontendTransactionTests(unittest.TestCase):
    def test_wisp_failure_recovers_then_valid_source_commits(self):
        subprocess.run(["make", "libmonad-compiler.a"], cwd=ROOT, check=True,
                       text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with tempfile.TemporaryDirectory(prefix="monadc-frontend-transaction-") as directory:
            executable = Path(directory) / "host"
            built = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-iquote", str(ROOT / "src"), str(FIXTURE / "host.c"),
                 str(ROOT / "libmonad-compiler.a"), "-lpthread", "-lm",
                 "-o", str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(built.returncode, 0, built.stdout)
            ran = subprocess.run([str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, (FIXTURE / "stdout").read_text())


if __name__ == "__main__":
    unittest.main()
