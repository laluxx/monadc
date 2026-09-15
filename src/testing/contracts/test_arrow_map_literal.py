"""Arrow-bearing braces are relations; hash maps retain #{} syntax."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


from src.testing.monad_binary import resolve_monad_binary
ROOT = Path(__file__).resolve().parents[3]
MONAD = resolve_monad_binary()


class ArrowRelationLiteralTests(unittest.TestCase):
    def test_lowercase_script_gets_main_module_and_relation_core_implicitly(self):
        source = """define likes :: {'a -> 'b}
{ 'alice -> 'pizza
  'bob -> 'sushi }
assert likes 'alice 'pizza
show who likes 'pizza
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.mon"
            output = Path(directory) / "test"
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
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout.strip(), "(alice)")

    def test_typed_symbol_relation_is_callable_for_membership_and_query(self):
        source = """import Data.Relation
define likes :: {'a -> 'b}
{ 'alice -> 'pizza
  'bob -> 'sushi
  'alice -> 'sushi }
show (likes 'alice 'pizza)
show (likes who 'pizza)
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Relation.mon"
            output = Path(directory) / "relation"
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
            ran = subprocess.run(
                [str(output)], text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout.splitlines(), ["True", "(alice)"])

    def test_all_supported_layouts_preserve_relations(self):
        source = """import Data.Relation

define compact
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

show (relates? compact :a 1)
show (relates? hanging :b 2)
show (relates? expanded :a 1)
show (relation-size compact) + (relation-size hanging) + (relation-size expanded)
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

    def test_hash_braces_remain_a_map(self):
        source = "define table #{:a 1 :b 2}\nshow Map? table\n"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Map.mon"
            output = Path(directory) / "map"
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
