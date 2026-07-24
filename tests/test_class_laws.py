import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()


class ClassLawTests(unittest.TestCase):
    def compile_source(self, source: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory(prefix="monadc-class-law-") as temp_name:
            temp = Path(temp_name)
            home = temp / "home"
            home.mkdir()
            path = temp / "Law.mon"
            path.write_text(source, encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(ROOT / "core")
            return subprocess.run(
                [str(MONAD), "--emit-json", str(path)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )

    def emit_json(self, source: str) -> list[dict]:
        with tempfile.TemporaryDirectory(prefix="monadc-class-law-") as temp_name:
            temp = Path(temp_name)
            home = temp / "home"
            home.mkdir()
            path = temp / "Law.mon"
            path.write_text(source, encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(ROOT / "core")
            result = subprocess.run(
                [str(MONAD), "--emit-json", str(path)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            return json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))

    def test_class_law_is_metadata_not_a_dictionary_method(self):
        ast = self.emit_json(
            """\
(class Semigroup a where
  (append) :: a -> a -> a
  (law associativity :: Eq a => a -> a -> a -> Bool
    (x y z) => (= (append (append x y) z) (append x (append y z)))))
"""
        )

        declaration = ast[0]
        self.assertEqual(declaration["type"], "class")
        self.assertEqual(
            declaration["methods"],
            [{"name": "append", "type": "a -> a -> a"}],
        )
        self.assertEqual(len(declaration["laws"]), 1)
        self.assertEqual(declaration["laws"][0]["name"], "associativity")
        self.assertEqual(
            declaration["laws"][0]["type"],
            "Eq a => a -> a -> a -> Bool",
        )
        self.assertEqual(declaration["laws"][0]["parameters"], ["x", "y", "z"])
        self.assertEqual(declaration["laws"][0]["body"]["type"], "list")

    def test_wisp_class_law_lowers_to_the_same_metadata(self):
        ast = self.emit_json(
            """\
class Semigroup a where
  append :: a -> a -> a
  law associativity :: Eq a => a -> a -> a -> Bool
    x y z -> append (append x y) z == append x (append y z)
"""
        )

        declaration = ast[0]
        self.assertEqual(
            declaration["methods"],
            [{"name": "append", "type": "a -> a -> a"}],
        )
        self.assertEqual(
            declaration["laws"][0]["name"],
            "associativity",
        )
        self.assertEqual(
            declaration["laws"][0]["parameters"],
            ["x", "y", "z"],
        )

    def test_wisp_law_requires_an_executable_clause(self):
        result = self.compile_source(
            """\
class Semigroup a where
  append :: a -> a -> a
  law associativity :: Eq a => a -> a -> a -> Bool
"""
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("law 'associativity' requires a property clause", result.stdout)

    def test_duplicate_law_names_are_rejected(self):
        result = self.compile_source(
            """\
class Eq a where
  eq? :: a -> a -> Bool
  law reflexive :: a -> Bool
    x -> eq? x x
  law reflexive :: a -> Bool
    x -> True
"""
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate law 'reflexive'", result.stdout)

    def test_law_result_must_be_bool(self):
        result = self.compile_source(
            """\
class Broken a where
  value :: a -> Int
  law not-a-property :: a -> Int
    x -> value x
"""
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("law 'not-a-property' must return Bool", result.stdout)

    def test_law_signature_arity_must_match_its_parameters(self):
        result = self.compile_source(
            """\
class Eq a where
  eq? :: a -> a -> Bool
  law reflexive :: a -> a -> Bool
    x -> eq? x x
"""
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "law 'reflexive' declares 2 arguments but its property clause binds 1",
            result.stdout,
        )

    def test_core_executable_law_inventory_is_structured_metadata(self):
        expected = {
            "Data/Eq.mon": {
                "Eq": {"reflexive", "symmetric", "transitive", "complement"},
            },
            "Data/Semigroup.mon": {
                "Semigroup": {"associativity"},
                "Monoid": {"left-identity", "right-identity"},
            },
            "Data/Enum.mon": {
                "Enum": {"successor-representation", "predecessor-representation"},
            },
            "Data/Ord.mon": {
                "Ord": {
                    "compare-consistency",
                    "minimum-consistency",
                    "maximum-consistency",
                },
            },
            "Numeric.mon": {
                "Ring": {
                    "add-associativity",
                    "add-commutativity",
                    "additive-identity",
                    "additive-inverse",
                    "mul-associativity",
                    "multiplicative-identity",
                    "left-distributivity",
                    "right-distributivity",
                },
            },
            "Text/Show.mon": {
                "Show": {"show-via-showsPrec"},
            },
            "Sequence.mon": {
                "Sequence": {
                    "concat-associativity",
                    "concat-length",
                    "reverse-involution",
                },
            },
        }

        for relative, classes in expected.items():
            with self.subTest(module=relative):
                source = (ROOT / "core" / "prelude" / relative).read_text(
                    encoding="utf-8"
                )
                declarations = self.emit_json(source)
                actual = {
                    declaration["name"]: {
                        law["name"] for law in declaration.get("laws", [])
                    }
                    for declaration in declarations
                    if declaration["type"] == "class"
                }
                for class_name, law_names in classes.items():
                    self.assertEqual(actual[class_name], law_names)


if __name__ == "__main__":
    unittest.main()
