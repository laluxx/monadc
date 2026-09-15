"""Transactional ownership of partially constructed frontend ASTs."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "src/testing/contracts/fixtures/embedding/branches/parser-transaction/atom.embedding.20.parser-transaction.ast-ledger"


class AstTransactionTests(unittest.TestCase):
    def test_partial_graph_unwinds_and_success_transfers(self):
        subprocess.run(["make", "libmonad-compiler.a"], cwd=ROOT, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        with tempfile.TemporaryDirectory(prefix="monadc-ast-transaction-") as directory:
            executable = Path(directory) / "host"
            built = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-iquote", str(ROOT / "src"),
                 str(FIXTURE / "host.c"), str(ROOT / "libmonad-compiler.a"),
                 "-lpthread", "-lm", "-o", str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(built.returncode, 0, built.stdout)
            ran = subprocess.run([str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, (FIXTURE / "stdout").read_text())

    def test_partial_graph_is_asan_clean(self):
        with tempfile.TemporaryDirectory(prefix="monadc-ast-asan-") as directory:
            executable = Path(directory) / "host"
            sources = ["features.c", "macro.c", "pmatch.c", "reader.c",
                       "reader_diagnostic.c", "reader_syntax.c", "types.c",
                       "wisp.c", "wisp_syntax_policy.c"]
            built = subprocess.run(
                ["cc", "-g", "-fsanitize=address", "-fno-omit-frame-pointer",
                 "-std=c99", "-D_GNU_SOURCE", "-iquote", str(ROOT / "src"),
                 str(FIXTURE / "host.c"), *[str(ROOT / name) for name in sources],
                 "-lpthread", "-lm", "-o", str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(built.returncode, 0, built.stdout)
            environment = os.environ.copy()
            # LeakSanitizer cannot attach under this test container's ptrace
            # policy; AddressSanitizer still proves UAF/double-free safety.
            environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
            ran = subprocess.run([str(executable)], cwd=ROOT, text=True,
                env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, (FIXTURE / "stdout").read_text())


if __name__ == "__main__":
    unittest.main()
