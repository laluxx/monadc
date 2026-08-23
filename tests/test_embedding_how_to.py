"""The public embedding tutorial compiles and runs against the installed SDK."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TUTORIAL = ROOT / "how_to/Embedding.c"
FIXTURE = ROOT / "tests/embedding/branches/surface/atom.embedding.32.surface.one-owner-host/host.c"


class EmbeddingHowToTests(unittest.TestCase):
    def test_tutorial_is_lsp_resolvable_without_compile_flags(self):
        checked = subprocess.run(
            ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
             "-fsyntax-only", str(TUTORIAL)], cwd=ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(checked.returncode, 0, checked.stdout)

    def test_tutorial_uses_only_installed_public_abi(self):
        with tempfile.TemporaryDirectory(prefix="monadc-embedding-how-to-") as directory:
            prefix = Path(directory) / "sdk"
            installed = subprocess.run(
                ["make", "install", f"PREFIX={prefix}"], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(installed.returncode, 0, installed.stdout)
            executable = Path(directory) / "embedding-tutorial"
            built = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(prefix / "include"), str(FIXTURE),
                 str(prefix / "lib/libmonad-embed.a"), "-pthread",
                 "-o", str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(built.returncode, 0, built.stdout)
            ran = subprocess.run([str(executable)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, (
                "double(21) = 42\n"
            ))

    def test_beginner_tutorial_stays_at_the_surface(self):
        source = TUTORIAL.read_text()
        self.assertLessEqual(len(source.splitlines()), 70)
        self.assertIn("include <monad/monad.h>", source)
        for internal in ("monad_runtime_", "monad_thread_",
                         "monad_compiler_", "monad_native_registration_"):
            self.assertNotIn(internal, source)


if __name__ == "__main__":
    unittest.main()
