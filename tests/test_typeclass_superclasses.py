import os
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MONAD = ROOT / "monad"


class TypeclassSuperclassTests(unittest.TestCase):
    def run_monad(self, source: str, output_name: str = "out"):
        with tempfile.TemporaryDirectory(prefix="monadc-superclass-") as td:
            root = Path(td)
            core = root / "core"
            home = root / "home"
            core.mkdir()
            home.mkdir()
            fixture = root / "case.mon"
            output = root / output_name
            fixture.write_text(textwrap.dedent(source).strip() + "\n", encoding="utf-8")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(core)
            env["HOME"] = str(home)
            result = subprocess.run(
                [str(MONAD), str(fixture), "-o", str(output)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            run_output = ""
            if result.returncode == 0:
                run = subprocess.run(
                    [str(output)],
                    cwd=ROOT,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    check=False,
                )
                run_output = run.stdout
                if run.returncode != 0:
                    self.fail(f"compiled program failed with {run.returncode}:\n{run.stdout}")
            return result, run_output

    def test_superclass_instance_requires_parent_instance(self):
        result, _ = self.run_monad(
            """
            (module Main)
            (class Eq a where (eq?) :: a -> a -> Bool)
            (class Eq a => Ord a where (lt?) :: a -> a -> Bool)
            (instance Ord Int where (lt? x y) => (< x y))
            (show 42)
            """
        )

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(
            "superclass constraint requires instance 'Eq Int' before 'Ord Int'",
            result.stdout,
        )

    def test_superclass_instance_accepts_parent_instance(self):
        result, run_output = self.run_monad(
            """
            (module Main)
            (class Eq a where (eq?) :: a -> a -> Bool)
            (class Eq a => Ord a where (lt?) :: a -> a -> Bool)
            (instance Eq Int where (eq? x y) => (= x y))
            (instance Ord Int where (lt? x y) => (< x y))
            (show 42)
            """
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(run_output.endswith("42\n"), run_output)

    def test_multiple_superclasses_require_all_parent_instances(self):
        result, _ = self.run_monad(
            """
            (module Main)
            (class Functor f where (fmap) :: Fn -> f -> f)
            (class Foldable f where (fold) :: f -> Int)
            (class (Functor f, Foldable f) => Traversable f where (traverse) :: Fn -> f -> f)
            (instance Functor Int where (fmap f x) => x)
            (instance Traversable Int where (traverse f x) => x)
            (show 42)
            """
        )

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(
            "superclass constraint requires instance 'Foldable Int' before 'Traversable Int'",
            result.stdout,
        )

    def test_instance_method_numeric_literal_uses_expected_type(self):
        result, run_output = self.run_monad(
            """
            (module Main)
            (class Num a where (negate) :: a -> a)
            (instance Num Int where (negate x) => (- 0 x))
            (show 42)
            """
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(run_output.endswith("42\n"), run_output)

    def test_nullary_instance_method_compiles_as_zero_arg_function(self):
        result, run_output = self.run_monad(
            """
            (module Main)
            (class Additive a where (zero) :: a)
            (instance Additive Int where (zero) => 0)
            (show (zero))
            """
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(run_output.endswith("0\n"), run_output)

    def test_prefix_instance_method_accepts_more_than_two_arguments(self):
        result, run_output = self.run_monad(
            """
            (module Main)
            (class Tri a where (tri) :: a -> a -> a -> a)
            (instance Tri Int where (tri x y z) => (+ (+ x y) z))
            (show (tri 10 12 20))
            """
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(run_output.endswith("42\n"), run_output)

    def test_subtype_operator_numeric_width_chain(self):
        result, run_output = self.run_monad(
            """
            (module Main)
            (if U8 <: U16
              (show 1)
              (show 0))
            """
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(run_output.endswith("1\n"), run_output)

    def test_subtype_operator_arbitrary_width_chain(self):
        result, run_output = self.run_monad(
            """
            (module Main)
            (if U4 <: U8
              (if U4 <: U16
                (if U4 <: Int
                  (if U8 <: U4
                    (show 0)
                    (show 1))
                  (show 0))
                (show 0))
              (show 0))
            """
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(run_output.endswith("1\n"), run_output)

    def test_subtype_operator_refinement_to_base(self):
        result, run_output = self.run_monad(
            """
            (module Main)
            (type Positive { x ∈ Int | (> x 0) })
            (if Positive <: Int
              (show 1)
              (show 0))
            """
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(run_output.endswith("1\n"), run_output)

    def test_subtype_operator_rejects_reverse_numeric_width(self):
        result, run_output = self.run_monad(
            """
            (module Main)
            (if U16 <: U8
              (show 1)
              (show 0))
            """
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(run_output.endswith("0\n"), run_output)

    def test_subtype_inherits_parent_typeclass_instance(self):
        result, run_output = self.run_monad(
            """
            (module Main)
            (class Eq a where (eq?) :: a -> a -> Bool)
            (instance Eq Int where (eq? x y) => (= x y))
            (define [x :: U8] 7)
            (if (eq? x 7)
              (show 1)
              (show 0))
            """
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(run_output.endswith("1\n"), run_output)

    def test_equals_operator_uses_eq_typeclass_instance(self):
        result, run_output = self.run_monad(
            """
            (module Main)
            (class Eq a where (eq?) :: a -> a -> Bool (not-eq?) :: a -> a -> Bool)
            (instance Eq Int where (eq? x y) => False (not-eq? x y) => True)
            (if (= 1 1)
              (show 1)
              (if (!= 1 1)
                (show 0)
                (show 2)))
            """
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(run_output.endswith("0\n"), run_output)

    def test_deriving_show_for_nullary_adt_constructor(self):
        result, run_output = self.run_monad(
            """
            (module Main)
            (class Show a where (render) :: a -> String)
            (data Color Red | Green | Blue deriving [Show])
            (show (render Red))
            """
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(run_output.endswith("Red\n"), run_output)


if __name__ == "__main__":
    unittest.main()
