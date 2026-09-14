"""Phase-1 contracts: context/typeclass.org::associated-strategy cycles 1-3."""
import unittest
import re
from pathlib import Path

import test_typeclass_superclasses as superclass_tests


class AssociatedStrategyTests(unittest.TestCase):
    run_monad = superclass_tests.TypeclassSuperclassTests.run_monad

    def test_associated_superclass_uses_strategy_not_state(self):
        for owner in ("Cursor", "Sequence", "BinaryTree", "Producer"):
            with self.subTest(owner=owner):
                result, output = self.run_monad(f"""
                (module Main)
                (class Evaluation e where)
                (instance Evaluation Bool where)
                (class Evaluation (Strategy s) => {owner} s where
                  type Strategy s
                  (observe) :: s -> s)
                (instance {owner} Int where
                  type Strategy Int = Bool
                  (observe x) => x)
                (show (observe 42))
                """)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertEqual(output, "42\n")

    def test_wisp_associated_strategy_and_element_specialize(self):
        result, output = self.run_monad("""
        module Main
        class Evaluation e where
        instance Evaluation Float
        class Evaluation (Strategy s) => Cursor s where
          type Element s
          type Strategy s
          observe :: s -> Element s
        instance Cursor Int
          type Element Int = Int
          type Strategy Int = Float
          observe x -> x
        show (observe 42)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42\n")

    def test_multiple_superclasses_reduce_independently(self):
        result, output = self.run_monad("""
        (module Main)
        (class Evaluation e where)
        (class Tagged s where)
        (instance Evaluation Float where)
        (instance Tagged Int where)
        (class (Tagged s, Evaluation (Strategy s)) => Cursor s where
          type Strategy s)
        (instance Cursor Int where type Strategy Int = Float)
        (show 42)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42\n")

    def test_core_semantics_have_no_compiler_name_dispatch(self):
        root = Path(__file__).resolve().parents[1]
        protected = ("Sequence", "Producer", "BinaryTree", "Text.Unicode",
                     "Text.Unicode.Properties", "Text.TerminalWidth", "Web.HTTP")
        for source in root.rglob("*.c"):
            if any(part in ("tests", "build", ".git") for part in source.parts):
                continue
            text = source.read_text()
            # This is the printed name of sequential QTT demand composition,
            # unrelated to the Core class. Keep the exception exact.
            if source == root / "qtt/compiler_module.c":
                text = text.replace(
                    'case QTT_DEMAND_RULE_SEQUENCE: return "Sequence";', "")
            literals = set(re.findall(r'"([^"\n]*)"', text))
            self.assertFalse(literals.intersection(protected), str(source))

    def test_unowned_associated_superclass_is_rejected(self):
        result, _ = self.run_monad("""
        (module Main)
        (class Evaluation e where)
        (class Evaluation (Foreign s) => Cursor s where type Strategy s)
        """)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("Foreign s", result.stdout)
        self.assertIn("owned associated type", result.stdout)

    def test_missing_strategy_instance_names_associated_owner(self):
        result, _ = self.run_monad("""
        (module Main)
        (class Evaluation e where)
        (instance Evaluation Int where)
        (class Evaluation (Strategy s) => Cursor s where type Strategy s)
        (instance Cursor Int where type Strategy Int = Bool)
        """)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("Evaluation Bool", result.stdout)
        self.assertIn("Strategy", result.stdout)
        self.assertIn("Cursor Int", result.stdout)

    def test_missing_associated_equation_is_rejected(self):
        result, _ = self.run_monad("""
        (module Main)
        (class Evaluation e where)
        (instance Evaluation Int where)
        (class Evaluation (Strategy s) => Cursor s where type Strategy s)
        (instance Cursor Int where)
        """)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("Strategy", result.stdout)
        self.assertIn("Cursor Int", result.stdout)

    def test_compound_instance_equations(self):
        for head in ("[Int]", "[a]", "(Box Int)", "(Box [Int])", "(Machine e a)"):
            with self.subTest(head=head):
                result, output = self.run_monad(f"""
                (module Main)
                (class Evaluation e where)
                (instance Evaluation Float where)
                (class Evaluation (Strategy s) => Cursor s where
                  type Strategy s)
                (instance Cursor {head} where type Strategy {head} = Float)
                (show 42)
                """)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertEqual(output, "42\n")

    def test_compound_strategy_equation_value(self):
        result, output = self.run_monad("""
        (module Main)
        (class Evaluation e where)
        (instance Evaluation (Box Float) where)
        (class Evaluation (Strategy s) => Cursor s where type Strategy s)
        (instance Cursor Int where type Strategy Int = (Box Float))
        (show 42)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42\n")

    def test_equation_head_must_match_instance(self):
        result, _ = self.run_monad("""
        (module Main)
        (class Cursor s where type Element s)
        (instance Cursor Int where type Element Float = Int)
        """)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("associated type 'Element'", result.stdout)
        self.assertIn("instance head 'Int'", result.stdout)

    def test_equation_requires_equals(self):
        result, _ = self.run_monad("""
        (module Main)
        (class Cursor s where type Element s)
        (instance Cursor Int where type Element Int Float)
        """)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("associated type 'Element'", result.stdout)
        self.assertIn("Expected '='", result.stdout)

    def test_compound_element_method_specializes(self):
        result, output = self.run_monad("""
        module Main
        class Cursor s where
          type Element s
          observe :: s -> Element s
        instance Cursor Int
          type Element Int = (Int, Int)
          observe x -> (x, x)
        show (observe 21)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "(21 21)\n")

    def test_wisp_compound_equation_and_omitted_owner(self):
        result, output = self.run_monad("""
        module Main
        class Evaluation e where
        instance Evaluation (Box Float)
        class Evaluation (Strategy s) => Cursor s where
          type Strategy s
        instance Cursor [Int]
          type Strategy = (Box Float)
        show 42
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42\n")

    def test_equations_are_owned_and_unique(self):
        for equations, expected in (
            ("type Foreign Int = Float", "unknown associated type 'Foreign'"),
            ("type Element Int = Int type Element Int = Float",
             "duplicate associated type 'Element'"),
        ):
            with self.subTest(equations=equations):
                result, _ = self.run_monad(f"""
                (module Main)
                (class Cursor s where type Element s)
                (instance Cursor Int where {equations})
                """)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(expected, result.stdout)
                self.assertIn("Cursor Int", result.stdout)

    def test_overlapping_associated_names_reduce_structurally(self):
        result, output = self.run_monad("""
        module Main
        class Cursor s where
          type Element s
          type TailElement s
          tail :: s -> TailElement s
        instance Cursor Int
          type Element Int = Int
          type TailElement Int = Float
          tail x -> 42.5
        show (tail 1)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42.5\n")

    def test_associated_reduction_does_not_rewrite_nominal_substrings(self):
        result, output = self.run_monad("""
        module Main
        data Elemental = Elemental Int
        class Cursor s where
          type Element s
          keep :: s -> Elemental
        instance Cursor Int
          type Element Int = Float
          keep x -> Elemental 42
        show 42
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42\n")

    def test_chained_associated_reduction_is_equation_order_independent(self):
        result, output = self.run_monad("""
        module Main
        class Cursor s where
          type First s
          type Second s
          observe :: s -> First s
        instance Cursor Int
          type Second Int = Float
          type First Int = (Second s)
          observe x -> 42.5
        show (observe 1)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42.5\n")

    def test_concrete_nullary_call_uses_associated_result(self):
        result, output = self.run_monad("""
        module Main
        class Cell s where
          type Value s
          value :: Value s
        instance Cell Int
          type Value Int = Float
          value -> 42.5
        show (value)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42.5\n")

    def test_associated_reduction_is_a_shared_typeclass_api(self):
        root = Path(__file__).resolve().parents[1]
        header = (root / "typeclass.h").read_text()
        source = (root / "typeclass.c").read_text()
        self.assertIn("tc_specialize_type_expression", header)
        self.assertIn("tc_specialize_type_expression", source)
        self.assertIn("tc_instance_method_result_type", header)
        self.assertIn("tc_instance_method_result_type", source)

    def test_constrained_generic_method_call_specializes_at_call_site(self):
        result, output = self.run_monad("""
        module Main
        class Convert a where
          convert :: a -> a
        instance Convert Int
          convert x -> x + 1
        define twice :: Convert a => a -> a
          x -> convert (convert x)
        show (twice 40)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42\n")

    def test_constrained_module_compiles_before_instances_exist(self):
        result, output = superclass_tests.TypeclassSuperclassTests.run_monad_modules(
            self,
            {"Lib.mon": """
                module Lib [Convert convert twice]
                class Convert a where
                  convert :: a -> a
                define twice :: Convert a => a -> a
                  x -> convert (convert x)
            """},
            """
                module Main
                import Lib
                instance Convert Int
                  convert x -> x + 1
                show (twice 40)
            """,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42\n")

    def test_multiline_constrained_signature_is_one_define(self):
        result, output = self.run_monad("""
        module Main
        class Marker a where
          mark :: a -> a
        instance Marker Int
          mark x -> x
        define choose :: Marker a =>
          a -> a
          x -> x
        show (choose 42)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42\n")

    def test_recursive_two_parameter_adt_preserves_constructor_arguments(self):
        result, output = self.run_monad("""
        module Main
        data Binary e a
          = Empty
          | Node a (e (Binary e a)) (e (Binary e a))
        class Eval e where
          defer :: (() -> a) -> e a
        define branch :: Eval e =>
          a -> (() -> Binary e a) -> (() -> Binary e a) -> Binary e a
          value left right -> Node value (defer left) (defer right)
        define repeat-binary :: Eval e => a -> Binary e a
          value ->
            branch value
              (lambda (_) (repeat-binary value))
              (lambda (_) (repeat-binary value))
        show 42
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42\n")


if __name__ == "__main__":
    unittest.main()
