"""Certified foreign ownership lowering and exact call-count evidence."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "src/testing/contracts/fixtures/qtt/branches/foreign-lowering/atom.qtt.11.foreign-lowering.exact-runtime-actions"


class QttForeignLoweringTests(unittest.TestCase):
    def test_certified_runtime_actions_and_assembly(self):
        with tempfile.TemporaryDirectory(prefix="monadc-foreign-lowering-") as directory:
            directory = Path(directory)
            executable = directory / "plan"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-iquote", str(ROOT / "src"), str(FIXTURE / "host.c"),
                 str(ROOT / "qtt/foreign_lowering.c"),
                 str(ROOT / "qtt/resource.c"), str(ROOT / "qtt/core.c"),
                 str(ROOT / "qtt/place.c"), str(ROOT / "qtt/quantity.c"),
                 "-o", str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, (FIXTURE / "stdout").read_text())

            assembly = directory / "lowered.s"
            result = subprocess.run(
                ["cc", "-std=c99", "-O2", "-fno-inline", "-S",
                 "-I", str(ROOT / "embed/include"),
                 str(FIXTURE / "lowered.c"), "-o", str(assembly)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            text = assembly.read_text()

            def body(name):
                match = re.search(rf"^{name}:\n(.*?)(?=^\s*\.size\s+{name},)",
                                  text, re.MULTILINE | re.DOTALL)
                self.assertIsNotNone(match, text)
                return match.group(1)

            self.assertNotIn("call", body("lowered_foreign_move"))
            self.assertEqual(body("lowered_foreign_drop").count(
                "monad_foreign_object_release"), 1)
            self.assertEqual(body("lowered_foreign_dup").count(
                "monad_foreign_object_retain_shared"), 1)


if __name__ == "__main__":
    unittest.main()
