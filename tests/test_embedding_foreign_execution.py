"""C -> compiler/QTT -> LLVM ORCv2 -> foreign runtime ownership proof."""

from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/embedding/branches/foreign-execution/atom.embedding.14.foreign-execution.orc-ownership"


class ForeignExecutionTests(unittest.TestCase):
    def test_orc_executes_certified_foreign_ownership(self):
        flags = shlex.split(subprocess.check_output(
            ["llvm-config", "--cflags"], text=True))
        libraries = shlex.split(subprocess.check_output(
            ["llvm-config", "--ldflags", "--libs", "core", "orcjit", "native"],
            text=True))
        with tempfile.TemporaryDirectory(prefix="monadc-foreign-execution-") as directory:
            executable = Path(directory) / "host"
            result = subprocess.run(
                ["cc", *flags, "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-iquote", str(ROOT), "-I", str(ROOT / "embed/include"),
                 str(FIXTURE / "host.c"), str(ROOT / "qtt/foreign_llvm.c"),
                 str(ROOT / "qtt/foreign_lowering.c"), str(ROOT / "qtt/resource.c"),
                 str(ROOT / "qtt/core.c"), str(ROOT / "qtt/place.c"),
                 str(ROOT / "qtt/quantity.c"), str(ROOT / "libmonad-compiler.a"),
                 str(ROOT / "libmonad-embed.a"), *libraries, "-pthread", "-lm", "-rdynamic",
                 "-o", str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, (FIXTURE / "stdout").read_text())


if __name__ == "__main__":
    unittest.main()
