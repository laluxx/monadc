"""Frontend transactions overlap instead of hiding behind a process mutex."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "src/testing/contracts/fixtures/embedding/branches/frontend-state/atom.embedding.23.frontend-state.parallel-entry"


class FrontendParallelTests(unittest.TestCase):
    def test_two_frontends_overlap(self):
        subprocess.run(["make", "libmonad-compiler.a"], cwd=ROOT, check=True,
                       text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with tempfile.TemporaryDirectory(prefix="monadc-frontend-parallel-") as directory:
            executable = Path(directory) / "host"
            built = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-iquote", str(ROOT / "src"),
                 str(FIXTURE / "host.c"), str(ROOT / "libmonad-compiler.a"),
                 str(ROOT / "libmonad-embed.a"), "-pthread", "-lm", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(built.returncode, 0, built.stdout)
            ran = subprocess.run([str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, (FIXTURE / "stdout").read_text())


if __name__ == "__main__":
    unittest.main()
