"""Regression contracts for constrained, higher-kinded instance templates."""
import unittest

from src.testing.contracts import test_typeclass_superclasses as superclass_tests


class GenericInstanceTemplateTests(unittest.TestCase):
    run_monad = superclass_tests.TypeclassSuperclassTests.run_monad

    def test_constrained_applied_instance_is_not_linked_as_concrete(self):
        result, output = self.run_monad("""
        module Main
        class Functor f where
          map :: (a -> b) -> f a -> f b
        class Functor f => Applicative f where
          pure :: a -> f a
          applyA :: f (a -> b) -> f a -> f b
        class Applicative e => Evaluation e where
          demand :: e a -> a
        data TotalDemand e a = TotalDemandValue (e a)
        instance Evaluation e => Functor (TotalDemand e)
          map f [TotalDemandValue value] -> TotalDemandValue (map f value)
        instance Evaluation e => Applicative (TotalDemand e)
          pure value -> TotalDemandValue (pure value)
          applyA [TotalDemandValue f] [TotalDemandValue value] ->
            TotalDemandValue (applyA f value)
        show 42
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42\n")

    def test_lazy_compiler_substrate_delays_and_forces_a_value(self):
        result, output = self.run_monad("""
        module Main
        show (force (delay (40 + 2)))
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42\n")

    def test_lazy_delay_captures_lexical_values(self):
        result, output = self.run_monad("""
        module Main
        define force-after x -> force (delay (x + 2))
        show (force-after 40)
        """)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output, "42\n")


if __name__ == "__main__":
    unittest.main()
