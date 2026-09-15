"""Certified foreign ownership actions become exact, verifier-clean LLVM IR."""

from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "src/testing/contracts/fixtures/qtt/branches/foreign-llvm/atom.qtt.12.foreign-llvm.verified-direct-calls"


class QttForeignLlvmTests(unittest.TestCase):
    def test_verified_direct_calls_without_wrappers(self):
        llvm_cflags = shlex.split(subprocess.check_output(
            ["llvm-config", "--cflags"], text=True))
        llvm_libs = shlex.split(subprocess.check_output(
            ["llvm-config", "--ldflags", "--libs", "core"], text=True))
        with tempfile.TemporaryDirectory(prefix="monadc-foreign-llvm-") as directory:
            executable = Path(directory) / "emit"
            command = (["cc", *llvm_cflags, "-std=c99", "-Wall", "-Wextra",
                        "-Werror", "-iquote", str(ROOT / "src"),
                        str(FIXTURE / "host.c"), str(ROOT / "qtt/foreign_llvm.c"),
                        str(ROOT / "qtt/foreign_lowering.c"),
                        str(ROOT / "qtt/resource.c"), str(ROOT / "qtt/core.c"),
                        str(ROOT / "qtt/place.c"), str(ROOT / "qtt/quantity.c"),
                        *llvm_libs, "-o", str(executable)])
            result = subprocess.run(command, cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn((FIXTURE / "stdout").read_text(), result.stdout)

            def body(name):
                match = re.search(rf"define [^\n]*@{name}\([^\n]*\) \{{\n(.*?)^\}}",
                                  result.stdout, re.MULTILINE | re.DOTALL)
                self.assertIsNotNone(match, result.stdout)
                return match.group(1)

            self.assertNotIn("call", body("foreign_move"))
            self.assertEqual(body("foreign_drop").count(
                "call void @monad_foreign_object_release"), 1)
            self.assertEqual(body("foreign_dup").count(
                "call void @monad_foreign_object_retain_shared"), 1)
            self.assertNotIn("RuntimeValue", result.stdout)
            self.assertNotIn("dispatch", result.stdout)


if __name__ == "__main__":
    unittest.main()
