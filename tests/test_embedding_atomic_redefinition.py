"""A native source image reloads as one live generation."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/embedding/branches/live-source/atom.embedding.39.live-source.atomic-redefinition"


class AtomicRedefinitionTests(unittest.TestCase):
    def test_native_batch_conflict_rolls_back_every_cell(self):
        with tempfile.TemporaryDirectory(prefix="monadc-batch-rollback-") as directory:
            executable = Path(directory) / "batch"
            built = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-iquote", str(ROOT),
                 str(FIXTURE / "batch.c"), str(ROOT / "libmonad-embed.a"),
                 "-pthread", "-o", str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(built.returncode, 0, built.stdout)
            ran = subprocess.run([str(executable)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)

    def test_installed_surface_redefines_complete_native_image(self):
        with tempfile.TemporaryDirectory(prefix="monadc-atomic-reload-") as directory:
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
