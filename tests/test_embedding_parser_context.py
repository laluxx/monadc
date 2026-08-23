"""Per-thread nested parser diagnostic recovery without output or exit."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/embedding/branches/parser-context/atom.embedding.17.parser-context.thread-local-recovery"


class ParserContextTests(unittest.TestCase):
    def test_thread_local_recovery_context(self):
        with tempfile.TemporaryDirectory(prefix="monadc-parser-context-") as directory:
            executable = Path(directory) / "host"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-iquote", str(ROOT), str(FIXTURE / "host.c"),
                 str(ROOT / "reader_diagnostic.c"), "-pthread", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, (FIXTURE / "stdout").read_text())


if __name__ == "__main__":
    unittest.main()
