"""Nominal and ownership-safe QTT foreign call contracts."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "src/testing/contracts/fixtures/qtt/branches/foreign-call/atom.qtt.09.foreign-call.nominal-transfer"


class QttForeignCallTests(unittest.TestCase):
    def test_nominal_authority_and_foreign_transfer(self):
        with tempfile.TemporaryDirectory(prefix="monadc-qtt-foreign-") as directory:
            executable = Path(directory) / "foreign-call"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-iquote", str(ROOT / "src"), str(FIXTURE / "host.c"),
                 str(ROOT / "qtt/type_identity.c"),
                 str(ROOT / "qtt/foreign_type.c"),
                 str(ROOT / "qtt/core.c"), str(ROOT / "qtt/quantity.c"),
                 str(ROOT / "qtt/resource.c"), str(ROOT / "qtt/place.c"),
                 str(ROOT / "qtt/call.c"), "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            environment = os.environ.copy()
            environment["ASAN_OPTIONS"] = "detect_leaks=0"
            result = subprocess.run([str(executable)], cwd=ROOT,
                env=environment, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, (FIXTURE / "stdout").read_text())


if __name__ == "__main__":
    unittest.main()
