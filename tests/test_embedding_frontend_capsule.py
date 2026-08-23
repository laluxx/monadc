"""Private frontend capsule and shared-library ABI regression."""

from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
ATOM = ROOT / "tests/embedding/branches/frontend-capsule/atom.embedding.19.frontend-capsule.hidden-pic"


class FrontendCapsuleTests(unittest.TestCase):
    def test_compiler_library_contains_frontend_without_exporting_it(self):
        subprocess.run(
            ["make", "libmonad-compiler.a", "libmonad-compiler.so"],
            cwd=ROOT, check=True, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

        archive = subprocess.run(
            ["ar", "t", "libmonad-compiler.a"], cwd=ROOT, check=True,
            text=True, stdout=subprocess.PIPE).stdout.splitlines()
        expected = {
            "frontend_features.o", "frontend_macro.o", "frontend_pmatch.o",
            "frontend_reader.o", "frontend_reader_diagnostic.o",
            "frontend_reader_syntax.o", "frontend_types.o",
            "frontend_wisp.o", "frontend_wisp_syntax_policy.o",
        }
        self.assertTrue(expected.issubset(set(archive)), archive)

        symbols = subprocess.run(
            ["nm", "-D", "--defined-only", "libmonad-compiler.so"],
            cwd=ROOT, check=True, text=True,
            stdout=subprocess.PIPE).stdout.splitlines()
        exported = [line.rsplit(maxsplit=1)[-1] for line in symbols if line.strip()]
        private = [name for name in exported if not name.startswith("monad_")]
        self.assertEqual(private, [], "private compiler ABI leaked: " + repr(private))

        self.assertTrue((ATOM / "contract.org").is_file())


if __name__ == "__main__":
    unittest.main()
