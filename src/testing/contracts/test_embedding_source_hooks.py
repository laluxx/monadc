"""Installed simple surface accepts owned strings, byte views, and files."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "src/testing/contracts/fixtures/embedding/branches/surface/atom.embedding.37.surface.source-hooks"


class SourceHookTests(unittest.TestCase):
    def test_installed_source_hooks_share_owned_transaction(self):
        with tempfile.TemporaryDirectory(prefix="monadc-source-hooks-") as directory:
            directory = Path(directory)
            prefix = directory / "sdk"
            source_file = directory / "file.mon"
            source_file.write_text("define from-file :: Int\n  7\n")
            installed = subprocess.run(
                ["make", "install", f"PREFIX={prefix}"], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(installed.returncode, 0, installed.stdout)
            executable = directory / "host"
            built = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(prefix / "include"), str(FIXTURE / "host.c"),
                 str(prefix / "lib/libmonad-compiler.a"),
                 str(prefix / "lib/libmonad-embed.a"), "-pthread", "-lm",
                 "-o", str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(built.returncode, 0, built.stdout)
            ran = subprocess.run([str(executable), str(source_file)], cwd=ROOT,
                                 text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, (FIXTURE / "stdout").read_text())


if __name__ == "__main__":
    unittest.main()
