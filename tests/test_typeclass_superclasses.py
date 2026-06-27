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
