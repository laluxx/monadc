"""Installed compiler SDK exposes immutable committed environment snapshots."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/embedding/branches/introspection/atom.embedding.30.introspection.environment-snapshot"


class EnvironmentSnapshotTests(unittest.TestCase):
    def test_installed_sdk_returns_stable_source_order_snapshot(self):
        with tempfile.TemporaryDirectory(prefix="monadc-environment-snapshot-") as directory:
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
