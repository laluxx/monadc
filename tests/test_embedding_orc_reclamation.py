"""Retired generated images are reclaimed after their final reader."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/embedding/branches/live-source/atom.embedding.41.live-source.orc-reclamation"


class OrcReclamationTests(unittest.TestCase):
    def test_installed_surface_reclaims_retired_images(self):
        with tempfile.TemporaryDirectory(prefix="monadc-orc-reclaim-") as directory:
            directory = Path(directory)
            prefix = directory / "sdk"
            installed = subprocess.run(
                ["make", "install", f"PREFIX={prefix}"], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(installed.returncode, 0, installed.stdout)
            llvm_flags = subprocess.check_output(
                ["llvm-config", "--ldflags", "--libs", "core", "orcjit",
                 "native", "passes", "--system-libs"], text=True).split()
            executable = directory / "host"
            built = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(prefix / "include"), str(FIXTURE / "host.c"),
                 str(prefix / "lib/libmonad-compiler.a"),
                 str(prefix / "lib/libmonad-embed.a"), "-pthread", "-lm", "-lgmp", "-lclang",
                 *llvm_flags, "-o", str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(built.returncode, 0, built.stdout)
            ran = subprocess.run([str(executable)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, (FIXTURE / "stdout").read_text())


if __name__ == "__main__":
    unittest.main()
