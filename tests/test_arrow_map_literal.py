"""Arrow-bearing braces are maps; arrow-free braces remain sets."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MONAD = ROOT / "monad"


class ArrowMapLiteralTests(unittest.TestCase):
    def test_all_supported_layouts_infer_and_run_as_maps(self):
        source = """define compact
{ :a -> 1
  :b -> 2 }

define hanging
 { :a -> 1
   :b -> 2 }

define expanded
  {
    :a -> 1
    :b -> 2
  }

show Map? compact
show Map? hanging
show Map? expanded
show (count compact) + (count hanging) + (count expanded)
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ArrowMaps.mon"
            output = Path(directory) / "arrow-maps"
            path.write_text(source)
            environment = os.environ.copy()
            environment["HOME"] = directory
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(MONAD), str(path), "-o", str(output)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, env=environment,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(output)], text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout.splitlines(), ["True", "True", "True", "6"])

    def test_arrow_free_braces_remain_a_set(self):
        source = "define values {1 2 3}\nshow Set? values\n"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Set.mon"
            output = Path(directory) / "set"
            path.write_text(source)
            environment = os.environ.copy()
            environment["HOME"] = directory
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(MONAD), str(path), "-o", str(output)], cwd=ROOT,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                env=environment,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run([str(output)], text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.stdout.strip(), "True")


if __name__ == "__main__":
    unittest.main()
