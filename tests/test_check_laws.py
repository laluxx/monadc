import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()


class CheckLawsTests(unittest.TestCase):
    def compile_laws_ir(self, source: str) -> tuple[subprocess.CompletedProcess[str], str]:
        with tempfile.TemporaryDirectory(prefix="monadc-check-laws-ir-") as temp_name:
            temp = Path(temp_name)
            home = temp / "home"
            home.mkdir()
            source_path = temp / "LawCheck.mon"
            output_path = temp / "law-check"
            source_path.write_text(source, encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(ROOT / "core")
            result = subprocess.run(
                [
                    str(MONAD),
                    str(source_path),
                    "--test",
                    "--emit-ir",
                    "-o",
                    str(output_path),
                ],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=30,
            )
            ir_path = source_path.with_suffix(".ll")
            ir = ir_path.read_text(encoding="utf-8") if ir_path.exists() else ""
            return result, ir

    def run_laws(self, source: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory(prefix="monadc-check-laws-") as temp_name:
            temp = Path(temp_name)
            home = temp / "home"
            home.mkdir()
            source_path = temp / "LawCheck.mon"
            source_path.write_text(source, encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(ROOT / "core")
            return subprocess.run(
                [str(MONAD), "test", str(source_path)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=30,
            )

    def test_finite_core_type_is_checked_exhaustively_in_source_order(self):
        result = self.run_laws(
            """\
import Data.Eq

module LawCheck []

tests
  check-laws Eq Bool
"""
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("\x1b[36;1mQuickCheck\x1b[0m  Eq Bool", result.stdout)
        self.assertIn(
            "exhaustive  •  finite set  •  source-order traversal",
            result.stdout,
        )
        self.assertIn(
            "\x1b[32m✓\x1b[0m  reflexive",
            result.stdout,
        )
        self.assertIn(
            "reflexive   a -> Bool",
            result.stdout,
        )
        self.assertIn(
            "\x1b[32;1mPASS\x1b[0m  4 properties · 18 cases checked · exhaustive",
            result.stdout,
        )

    def test_laws_block_accepts_seeded_law_subkeywords(self):
        result = self.run_laws(
            """\
import Data.Eq
import Test.QuickCheck

module LawCheck []

tests
  laws Eq Bool
    seeded Eq Int 7 42
    seeded Eq Int 3 17
"""
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout.count("QuickCheck\x1b[0m  Eq Bool"), 1)
        self.assertEqual(result.stdout.count("QuickCheck\x1b[0m  Eq Int"), 2)
        self.assertIn("28 cases checked · seed 42", result.stdout)
        self.assertIn("12 cases checked · seed 17", result.stdout)

    def test_seeded_law_prefix_uses_deterministic_defaults(self):
        result = self.run_laws(
            """\
import Data.Eq
import Test.QuickCheck

module LawCheck []

tests
  seeded law Eq Int
"""
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("QuickCheck\x1b[0m  Eq Int", result.stdout)
        self.assertIn("400 cases checked · seed 0", result.stdout)

    def test_seeded_remains_available_as_an_ordinary_identifier(self):
        result = self.run_laws(
            """\
module LawCheck []

define seeded 42

tests
  assert-eq seeded 42 "seeded is contextual, not globally reserved"
"""
        )

        self.assertEqual(result.returncode, 0, result.stdout)

    def test_non_finite_type_requires_generation_evidence(self):
        result = self.run_laws(
            """\
import Data.Eq

module LawCheck []

tests
  check-laws Eq Int
"""
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "cannot check laws for infinite or opaque type 'Int' without Arbitrary evidence",
            result.stdout,
        )

    def test_arbitrary_instance_checks_an_infinite_type_with_replayable_seed(self):
        result = self.run_laws(
            """\
import Data.Eq
import Test.QuickCheck

module LawCheck []

tests
  check-laws Eq Int
"""
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("\x1b[36;1mQuickCheck\x1b[0m  Eq Int", result.stdout)
        self.assertIn("seed 0  •  deterministic replay", result.stdout)
        self.assertIn("reflexive   a -> Bool", result.stdout)
        self.assertIn("symmetric   a -> a -> Bool", result.stdout)
        self.assertIn("transitive  a -> a -> a -> Bool", result.stdout)
        self.assertIn("complement  a -> a -> Bool", result.stdout)

    def test_core_enum_and_ord_laws_execute(self):
        result = self.run_laws(
            """\
import Data.Enum
import Data.Ord
import Test.QuickCheck

module LawCheck []

tests
  check-laws-seeded Enum Int 100 17
  check-laws-seeded Ord Int 100 17
"""
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("\x1b[36;1mQuickCheck\x1b[0m  Enum Int", result.stdout)
        self.assertIn("successor-representation", result.stdout)
        self.assertIn("\x1b[36;1mQuickCheck\x1b[0m  Ord Int", result.stdout)
        self.assertIn("compare-consistency", result.stdout)
        self.assertIn("minimum-consistency", result.stdout)
        self.assertIn("maximum-consistency", result.stdout)

    def test_exact_ring_laws_execute_without_claiming_ieee_float_is_a_ring(self):
        result = self.run_laws(
            """\
import Numeric
import Test.QuickCheck

module LawCheck []

tests
  check-laws-seeded Ring Int 1000 23
"""
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("\x1b[36;1mQuickCheck\x1b[0m  Ring Int", result.stdout)
        self.assertIn(
            "\x1b[32m✓\x1b[0m  add-associativity",
            result.stdout,
        )
        self.assertIn(
            "add-associativity        Eq a => a -> a -> a -> Bool  1000 cases",
            result.stdout,
        )
        self.assertIn(
            "\x1b[32;1mPASS\x1b[0m  8 properties · 8000 cases checked · seed 23",
            result.stdout,
        )
        for law in (
            "add-associativity",
            "add-commutativity",
            "additive-identity",
            "additive-inverse",
            "mul-associativity",
            "multiplicative-identity",
            "left-distributivity",
            "right-distributivity",
        ):
            self.assertIn(law, result.stdout)

        float_result = self.run_laws(
            """\
import Numeric
import Test.QuickCheck

module LawCheck []

tests
  check-laws-seeded Ring Float 10 23
"""
        )
        self.assertNotEqual(float_result.returncode, 0)
        self.assertIn(
            "check-laws requires instance 'Ring Float'",
            float_result.stdout,
        )
    def test_seeded_form_controls_case_count_and_replay_seed(self):
        result = self.run_laws(
            """\
import Data.Eq
import Test.QuickCheck

module LawCheck []

tests
  check-laws-seeded Eq Int 7 42
"""
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("seed 42  •  deterministic replay", result.stdout)
        self.assertIn("reflexive   a -> Bool", result.stdout)
        self.assertIn("complement  a -> a -> Bool", result.stdout)

    def test_seeded_form_preserves_all_64_seed_bits(self):
        result = self.run_laws(
            """\
import Data.Eq
import Test.QuickCheck

module LawCheck []

tests
  check-laws-seeded Eq Int 1 0xffffffffffffffff
"""
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("seed -1  •  deterministic replay", result.stdout)
        self.assertIn("reflexive   a -> Bool", result.stdout)

    def test_generated_failure_reports_exact_case_seed_and_size(self):
        result = self.run_laws(
            """\
import Test.QuickCheck

module LawCheck []

class Never a where
  reject :: a -> Bool

  law rejected :: a -> Bool
    x -> reject x

instance Never Int
  reject x -> False

tests
  check-laws-seeded Never Int 7 42
"""
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("\x1b[31m✗\x1b[0m  rejected", result.stdout)
        self.assertIn("\x1b[31;1mFAILED\x1b[0m", result.stdout)
        self.assertIn("counterexample  [", result.stdout)
        self.assertIn("replay  check-laws-seeded", result.stdout)
        self.assertIn("Law failed: Never Int.rejected", result.stdout)
        self.assertIn("case=0 seed=42 size=0", result.stdout)
        self.assertIn(
            "replay: check-laws-seeded Never Int 1 42",
            result.stdout,
        )

    def test_int_shrinking_is_logarithmic_and_strictly_progressing(self):
        result = self.run_laws(
            """\
import Test.QuickCheck

module LawCheck []

tests
  assert-eq (show (shrink 10)) "(0 5 8 9)" "positive Int shrink tree"
  assert-eq (show (shrink -10)) "(0 -5 -8 -9)" "negative Int shrink tree"
  assert-eq (show (shrink 0)) "()" "zero is already minimal"
"""
        )

        self.assertEqual(result.returncode, 0, result.stdout)

    def test_generated_counterexample_is_greedily_shrunk_to_a_local_minimum(self):
        result = self.run_laws(
            """\
module LawCheck []

class Arbitrary a where
  arbitrary :: Int -> Int -> a
  shrink :: a -> [a]

class Small a where
  small? :: a -> Bool

  law small :: a -> Bool
    x -> small? x

define shrink-large :: Int -> [Int]
  10 -> list 0 5 8 9
  5  -> list 0 2 4
  4  -> list 0 2 3
  _  -> list

instance Small Int
  small? x -> x <= 3

instance Arbitrary Int
  arbitrary seed size -> 10
  shrink x -> shrink-large x

tests
  check-laws-seeded Small Int 1 42
"""
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("counterexample  [4]", result.stdout)
        self.assertIn("shrinking       8 evaluations", result.stdout)

    def test_non_progressing_shrink_candidates_are_ignored(self):
        result = self.run_laws(
            """\
module LawCheck []

class Arbitrary a where
  arbitrary :: Int -> Int -> a
  shrink :: a -> [a]

class Never a where
  reject :: a -> Bool

  law rejected :: a -> Bool
    x -> reject x

define stuck :: Int -> [Int]
  x -> __rt_prepend x []

instance Arbitrary Int
  arbitrary seed size -> 7
  shrink x -> stuck x

instance Never Int
  reject x -> False

tests
  check-laws-seeded Never Int 1 0
"""
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("counterexample  [7]", result.stdout)
        self.assertIn("shrinking       0 evaluations", result.stdout)

    def test_splitmix64_core_generator_matches_golden_vectors(self):
        result = self.run_laws(
            """\
import Test.QuickCheck

module LawCheck []

tests
  assert-eq
    (quickcheck-mix64 (quickcheck-next-seed 0))
    0xe220a8397b1dcdaf
    "SplitMix64 seed 0"
  assert-eq
    (quickcheck-mix64 (quickcheck-next-seed 1))
    0x910a2dec89025cc1
    "SplitMix64 seed 1"
  assert-eq
    (quickcheck-mix64 (quickcheck-next-seed 42))
    0xbdd732262feb6e95
    "SplitMix64 seed 42"
"""
        )

        self.assertEqual(result.returncode, 0, result.stdout)

    def test_failing_finite_law_reports_the_first_source_order_counterexample(self):
        result = self.run_laws(
            """\
module LawCheck []

type Bit {Zero One}

class Always a where
  accept :: a -> Bool

  law accepted :: a -> Bool
    x -> accept x

instance Always Bit
  accept Zero -> False
  accept One  -> True

tests
  check-laws Always Bit
"""
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("\x1b[31m✗\x1b[0m  accepted", result.stdout)
        self.assertIn("Law failed: Always Bit.accepted", result.stdout)
        self.assertIn("counterexample: [Zero]", result.stdout)

    def test_finite_laws_specialize_without_runtime_property_closures(self):
        result, ir = self.compile_laws_ir(
            """\
import Data.Eq

module LawCheck []

tests
  check-laws Eq Bool
"""
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("__anon_val_", ir)
        self.assertNotIn("law_unbox", ir)
        self.assertIn("law_case_fail", ir)

    def test_sample_count_does_not_expand_randomized_law_ir(self):
        def source(count: int) -> str:
            return f"""\
import Data.Eq
import Test.QuickCheck

module LawCheck []

tests
  check-laws-seeded Eq Int {count} 42
"""

        small_result, small_ir = self.compile_laws_ir(source(10))
        large_result, large_ir = self.compile_laws_ir(source(100))
        self.assertEqual(small_result.returncode, 0, small_result.stdout)
        self.assertEqual(large_result.returncode, 0, large_result.stdout)
        self.assertLess(
            len(large_ir),
            int(len(small_ir) * 1.20),
            "sample count must be a runtime loop bound, not compile-time unrolling",
        )


if __name__ == "__main__":
    unittest.main()
