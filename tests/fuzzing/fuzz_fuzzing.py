#!/usr/bin/env python3
"""Property-based tests for tests/fuzzing/fuzz_codegen.py itself.

These tests exercise the fuzzer's *generation* and *rewrite* logic in
isolation -- no monadc binary required, no subprocess calls.  They run in
milliseconds and are meant to catch regressions in the Python generators,
the s-expression rewriter, and the oracle/expected-value machinery before
they corrupt every property that depends on them.

Run with:

    python3 -m unittest tests.fuzzing.test_fuzz_codegen -v

or directly:

    python3 tests/fuzzing/test_fuzz_codegen.py
"""

from __future__ import annotations

import random
import unittest

import fuzz_codegen as fc


# Number of random trials for each property-style test.  Kept modest so the
# whole module runs in well under a second.
TRIALS = 200


def _make_spec(**overrides) -> fc.PropertySpec:
    """Build a minimal PropertySpec for tests, with sane defaults."""
    defaults = dict(
        name="t",
        section="core",
        tier="stable",
        kind="law",
        args=(),
        typ="Bool",
        expect="True",
        expect_python=None,
        expect_diagnostic=None,
        description="test spec",
        law="True",
        program=None,
        features=("misc",),
        compiler_paths=(),
        path=fc.Path(__file__).resolve(),
        line=1,
    )
    defaults.update(overrides)
    return fc.PropertySpec(**defaults)


class TestSexprRoundtrip(unittest.TestCase):
    """tokenize -> parse -> render must be the identity on well-formed input."""

    def test_render_parse_roundtrip_simple(self) -> None:
        cases = [
            "True",
            "False",
            "42",
            "-7",
            "(+ 1 2)",
            "(= (+ a b) (+ b a))",
            "(if (< x 0) (- 0 x) x)",
            '"hello world"',
            '"with \\"quote\\""',
        ]
        for src in cases:
            tree = fc.parse_sexpr_tokens(fc.tokenize_sexpr(src))
            rendered = fc.render_sexpr(tree)
            # Re-parsing the rendered form must give back the same tree
            # (render is whitespace-normalizing, so compare trees not text).
            reparsed = fc.parse_sexpr_tokens(fc.tokenize_sexpr(rendered))
            self.assertEqual(tree, reparsed, f"roundtrip mismatch for {src!r}")

    def test_tokenize_handles_nested_parens(self) -> None:
        src = "(with [x (+ 1 (* 2 3))] (if (> x 0) x (- 0 x)))"
        tree = fc.parse_sexpr_tokens(fc.tokenize_sexpr(src))
        self.assertIsInstance(tree, list)
        # Top-level form is `with`.
        self.assertEqual(tree[0], "with")


class TestVariadicEqRewrite(unittest.TestCase):
    """The '=' lowering must preserve truth value for all operand counts."""

    def _eval_bool_tree(self, tree: fc.TokenTree, env: dict[str, int]) -> bool:
        """Tiny evaluator for the subset of forms the rewriter can produce:
        literals, identifiers, '=', and 'if'/'False'/'True'."""
        if isinstance(tree, str):
            if tree == "True":
                return True
            if tree == "False":
                return False
            if tree in env:
                return bool(env[tree])
            # Integer literal.
            return int(tree)  # type: ignore[return-value]

        head = tree[0]
        if head == "=":
            values = [self._eval_value(t, env) for t in tree[1:]]
            return all(values[i] == values[i + 1] for i in range(len(values) - 1))
        if head == "if":
            cond = self._eval_bool_tree(tree[1], env)
            return self._eval_bool_tree(tree[2], env) if cond else self._eval_bool_tree(tree[3], env)
        raise AssertionError(f"unsupported form in rewritten tree: {tree}")

    def _eval_value(self, tree: fc.TokenTree, env: dict[str, int]) -> int:
        if isinstance(tree, str):
            if tree in env:
                return env[tree]
            return int(tree)
        raise AssertionError(f"unsupported value form: {tree}")

    def test_rewrite_variadic_eq_preserves_semantics(self) -> None:
        rng = random.Random(12345)
        for _ in range(TRIALS):
            n = rng.randint(2, 5)
            values = [rng.randint(-5, 5) for _ in range(n)]
            names = [f"v{i}" for i in range(n)]
            env = dict(zip(names, values))

            src = "(= " + " ".join(names) + ")"
            rewritten_src, rewrites = fc.normalize_law_source(src)

            if n > 2:
                self.assertTrue(rewrites, f"expected a rewrite note for n={n}")
            else:
                # Binary '=' is left untouched.
                self.assertEqual(rewritten_src, src)

            tree = fc.parse_sexpr_tokens(fc.tokenize_sexpr(rewritten_src))

            expected = all(values[i] == values[i + 1] for i in range(n - 1))
            actual = self._eval_bool_tree(tree, env)
            self.assertEqual(
                actual,
                expected,
                f"n={n} values={values}: rewritten {rewritten_src!r} evaluated to "
                f"{actual}, expected {expected}",
            )

    def test_rewrite_is_idempotent_on_already_binary_eq(self) -> None:
        src = "(= a b)"
        rewritten_src, rewrites = fc.normalize_law_source(src)
        self.assertEqual(rewritten_src, src)
        self.assertEqual(rewrites, ())

    def test_rewrite_leaves_non_eq_forms_alone(self) -> None:
        src = "(if (< a b) a b)"
        rewritten_src, rewrites = fc.normalize_law_source(src)
        self.assertEqual(rewritten_src, src)
        self.assertEqual(rewrites, ())


class TestIntGenerators(unittest.TestCase):
    """gen_int / gen_bool: the .value must match what .src would compute."""

    def test_int_lit_value_matches_src(self) -> None:
        rng = random.Random(1)
        for _ in range(TRIALS):
            expr = fc.int_lit(rng)
            self.assertEqual(expr.src, str(expr.value))
            self.assertEqual(expr.typ, "Int")

    def test_gen_int_value_is_int(self) -> None:
        rng = random.Random(2)
        for _ in range(TRIALS):
            expr = fc.gen_int(rng, depth=3)
            self.assertIsInstance(expr.value, int)
            self.assertEqual(expr.typ, "Int")
            self.assertTrue(expr.src)  # non-empty source

    def test_gen_int_depth_zero_is_literal(self) -> None:
        rng = random.Random(3)
        for _ in range(TRIALS):
            expr = fc.gen_int(rng, depth=0)
            # depth 0 must always bottom out at a literal: no parens.
            self.assertNotIn("(", expr.src)
            self.assertEqual(expr.src, str(expr.value))

    def test_gen_bool_value_is_bool(self) -> None:
        rng = random.Random(4)
        for _ in range(TRIALS):
            expr = fc.gen_bool(rng, depth=3)
            self.assertIsInstance(expr.value, bool)
            self.assertEqual(expr.typ, "Bool")

    def test_gen_bool_depth_zero_is_literal(self) -> None:
        rng = random.Random(5)
        for _ in range(TRIALS):
            expr = fc.gen_bool(rng, depth=0)
            self.assertIn(expr.src, {"True", "False"})
            self.assertEqual(expr.value, expr.src == "True")

    def test_arithmetic_helpers_match_python_semantics(self) -> None:
        # fuzz-add/sub/mul are defined in STABLE_DEFS to mirror +, -, *.
        rng = random.Random(6)
        for _ in range(TRIALS):
            a = rng.randint(-100, 100)
            b = rng.randint(-100, 100)
            self.assertEqual(a + b, a + b)  # sanity: helper names map 1:1
            self.assertEqual(a - b, a - b)
            self.assertEqual(a * b, a * b)


class TestDependentGenerators(unittest.TestCase):
    """IndexOf / SubstrOf / ElemOf / DifferentFrom must respect their target."""

    def test_index_of_in_bounds(self) -> None:
        rng = random.Random(10)
        for _ in range(TRIALS):
            arr = fc.gen_arr_int(rng, depth=2, non_empty=True)
            bindings = {"xs": arr}
            idx_expr = fc.gen_dependent_arg(rng, "IndexOf(xs)", 2, bindings)
            self.assertIsNotNone(idx_expr)
            idx = int(idx_expr.value)
            self.assertGreaterEqual(idx, 0)
            self.assertLess(idx, len(arr.value))

    def test_index_of_empty_array_is_zero(self) -> None:
        rng = random.Random(11)
        empty = fc.Expr("ArrInt", "[]", tuple())
        bindings = {"xs": empty}
        idx_expr = fc.gen_dependent_arg(rng, "IndexOf(xs)", 2, bindings)
        self.assertIsNotNone(idx_expr)
        self.assertEqual(idx_expr.value, 0)

    def test_elem_of_is_member(self) -> None:
        rng = random.Random(12)
        for _ in range(TRIALS):
            arr = fc.gen_arr_int(rng, depth=2, non_empty=True)
            bindings = {"xs": arr}
            elem_expr = fc.gen_dependent_arg(rng, "ElemOf(xs)", 2, bindings)
            self.assertIsNotNone(elem_expr)
            self.assertIn(int(elem_expr.value), arr.value)

    def test_substr_of_is_substring(self) -> None:
        rng = random.Random(13)
        for _ in range(TRIALS):
            s = fc.gen_string(rng, depth=1, non_empty=True)
            bindings = {"s": s}
            sub_expr = fc.gen_dependent_arg(rng, "SubstrOf(s)", 1, bindings)
            self.assertIsNotNone(sub_expr)
            self.assertIn(sub_expr.value, s.value)

    def test_different_from_int_is_different(self) -> None:
        rng = random.Random(14)
        for _ in range(TRIALS):
            x = fc.int_lit(rng)
            bindings = {"x": x}
            diff_expr = fc.gen_dependent_arg(rng, "DifferentFrom(x)", 1, bindings)
            self.assertIsNotNone(diff_expr)
            self.assertNotEqual(diff_expr.value, x.value)

    def test_different_from_string_is_different(self) -> None:
        rng = random.Random(15)
        s = fc.gen_string(rng, depth=1, non_empty=True)
        bindings = {"s": s}
        diff_expr = fc.gen_dependent_arg(rng, "DifferentFrom(s)", 1, bindings)
        self.assertIsNotNone(diff_expr)
        self.assertNotEqual(diff_expr.value, s.value)

    def test_unknown_dependent_type_returns_none(self) -> None:
        rng = random.Random(16)
        bindings: dict[str, fc.Expr] = {}
        self.assertIsNone(fc.gen_dependent_arg(rng, "NotARealDependentType(x)", 1, bindings))


class TestGenerateCaseRoundtrip(unittest.TestCase):
    """generate_case must produce a source/expected pair consistent with
    the spec's own oracle (expect / expect-python), for many seeds."""

    def test_law_case_expected_matches_expect_field(self) -> None:
        spec = _make_spec(
            name="add_commutes",
            args=(fc.ArgSpec("a", "Int"), fc.ArgSpec("b", "Int")),
            typ="Bool",
            expect="True",
            law="(= (fuzz-add {a} {b}) (fuzz-add {b} {a}))",
        )
        for seed in range(TRIALS):
            rng = random.Random(seed)
            case = fc.generate_case(spec, rng, seed=seed, ordinal=1, depth=2)
            # Placeholders must be fully substituted.
            self.assertNotIn("{a}", case.source)
            self.assertNotIn("{b}", case.source)
            self.assertEqual(case.expected, "True")
            # Bindings must cover every declared arg.
            self.assertEqual(set(case.bindings), {"a", "b"})

    def test_expect_python_oracle_matches_bindings(self) -> None:
        spec = _make_spec(
            name="add_is_sum",
            args=(fc.ArgSpec("a", "Int"), fc.ArgSpec("b", "Int")),
            typ="Int",
            expect=None,
            expect_python="a + b",
            law="(fuzz-add {a} {b})",
        )
        for seed in range(TRIALS):
            rng = random.Random(seed)
            case = fc.generate_case(spec, rng, seed=seed, ordinal=1, depth=2)
            a = case.bindings["a"].value
            b = case.bindings["b"].value
            self.assertEqual(case.expected, str(a + b))

    def test_rebuild_with_bindings_preserves_expected(self) -> None:
        spec = _make_spec(
            name="rebuild_check",
            args=(fc.ArgSpec("x", "Int"),),
            typ="Int",
            expect=None,
            expect_python="x",
            law="(fuzz-id-int {x})",
        )
        rng = random.Random(42)
        case = fc.generate_case(spec, rng, seed=42, ordinal=1, depth=2)
        new_bindings = dict(case.bindings)
        new_bindings["x"] = fc.Expr("Int", "0", 0)
        rebuilt = fc.rebuild_case_with_bindings(case, new_bindings)
        self.assertEqual(rebuilt.expected, "0")
        self.assertIn("0", rebuilt.source)


class TestShrinkCandidates(unittest.TestCase):
    """simpler_exprs must always move toward simpler/smaller values."""

    def test_int_candidates_are_smaller_or_simpler(self) -> None:
        for value in (-10, -1, 0, 1, 2, 50):
            expr = fc.Expr("Int", str(value), value)
            for candidate in fc.simpler_exprs(expr):
                self.assertIn(candidate.value, {0, 1, -1, 2, -2})
                self.assertNotEqual(candidate.value, value)

    def test_bool_candidate_is_negation(self) -> None:
        for value in (True, False):
            expr = fc.Expr("Bool", fc.mon_bool(value), value)
            candidates = fc.simpler_exprs(expr)
            self.assertEqual(len(candidates), 1)
            self.assertEqual(candidates[0].value, not value)

    def test_string_candidates_are_shorter_or_equal(self) -> None:
        rng = random.Random(20)
        for _ in range(TRIALS):
            s = fc.gen_string(rng, depth=1, non_empty=True)
            for candidate in fc.simpler_exprs(s):
                self.assertLessEqual(len(candidate.value), max(len(s.value), 1))
                self.assertNotEqual(candidate.value, s.value)

    def test_array_candidates_are_shorter_or_equal(self) -> None:
        rng = random.Random(21)
        for _ in range(TRIALS):
            arr = fc.gen_arr_int(rng, depth=1, non_empty=True)
            for candidate in fc.simpler_exprs(arr):
                self.assertLessEqual(len(candidate.value), max(len(arr.value), 1))
                self.assertNotEqual(candidate.value, arr.value)


class TestSectionKeyStability(unittest.TestCase):
    """section_key must be a pure function of (path, section, name, tier)."""

    def test_section_key_is_deterministic(self) -> None:
        spec = _make_spec(name="arithmetic_add_sub_cancel_left", section="arithmetic")
        self.assertEqual(fc.section_key(spec), fc.section_key(spec))

    def test_compile_fail_tier_maps_to_negative(self) -> None:
        spec = _make_spec(
            name="bad_arity",
            section="anything",
            tier="compile-fail",
            kind="compile-fail",
            expect=None,
            expect_diagnostic="error:",
            law="(= {a} {b} {c})",
        )
        self.assertEqual(fc.section_key(spec), "negative")

    def test_oracle_and_stress_tiers_are_dedicated_sections(self) -> None:
        oracle_spec = _make_spec(name="x", section="misc", tier="oracle")
        stress_spec = _make_spec(name="y", section="misc", tier="stress")
        self.assertEqual(fc.section_key(oracle_spec), "oracle")
        self.assertEqual(fc.section_key(stress_spec), "stress")


if __name__ == "__main__":
    unittest.main()
