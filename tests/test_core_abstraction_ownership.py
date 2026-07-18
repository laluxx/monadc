import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def source(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class CoreAbstractionOwnershipTests(unittest.TestCase):
    def test_bool_is_a_core_owned_finite_type_set(self):
        bool_core = source("core/prelude/Data/Bool.mon")
        infer_c = source("infer.c")
        dep_c = source("dep.c")
        types_c = source("types.c")

        self.assertIn("type Bool {True False}", bool_core)
        self.assertNotIn('strcmp(ast->symbol, "True")', infer_c)
        self.assertNotIn('strcmp(ast->symbol, "False")', infer_c)
        self.assertNotIn('dep_env_declare(env, "Bool"', dep_c)
        self.assertNotIn('dep_env_define(env, "True"', dep_c)
        self.assertNotIn('dep_env_define(env, "False"', dep_c)
        self.assertNotIn(
            'if (strcmp(name, "Bool")    == 0) return type_bool();',
            types_c,
        )

    def test_numeric_typeclass_methods_have_no_concrete_module_copies(self):
        forbidden = ("inc", "dec", "double", "square", "cube", "abs", "signum")
        for module in ("core/prelude/Data/Int.mon", "core/prelude/Data/Float.mon"):
            text = source(module)
            for name in forbidden:
                self.assertNotRegex(
                    text,
                    rf"(?m)^method\s+{re.escape(name)}\s+::",
                    f"{module} duplicates canonical Numeric.{name}",
                )

    def test_numeric_has_no_parallel_additive_or_multiplicative_algebra(self):
        numeric = source("core/prelude/Numeric.mon")
        self.assertNotRegex(numeric, r"(?m)^class\s+(?:Additive|Multiplicative)\s+")
        self.assertNotRegex(numeric, r"(?m)^\s*(?:plus|minus|times)\s+::")

    def test_integral_methods_are_not_reimplemented_by_math_or_data_int(self):
        self.assertNotRegex(source("core/Math.mon"), r"(?m)^define\s+(even\?|odd\?|gcd|lcm)\s+::")
        self.assertNotRegex(source("core/prelude/Data/Int.mon"), r"(?m)^method\s+(even\?|odd\?)\s+::")

    def test_ordering_algorithms_have_one_owner(self):
        for module in ("core/Math.mon", "core/prelude/Data/Int.mon", "core/prelude/Data/Float.mon"):
            text = source(module)
            self.assertNotRegex(text, r"(?m)^(?:define|method)\s+(?:min|max|clamp)\s+::")

        ord_source = source("core/prelude/Data/Ord.mon")
        self.assertRegex(ord_source, r"(?m)^class\s+Ord\s+a\s+where")
        self.assertRegex(ord_source, r"(?m)^\s*clamp\s+::")

    def test_enum_deriving_uses_the_core_defaults(self):
        codegen = source("codegen.c")
        self.assertNotIn('strdup("succ")', codegen)
        self.assertNotIn('strdup("pred")', codegen)
        self.assertNotIn("derive_enum_step_nullary_lambda", codegen)

        enum_source = source("core/prelude/Data/Enum.mon")
        self.assertRegex(enum_source, r"(?m)^\s*succ\s+::")
        self.assertRegex(enum_source, r"(?m)^\s*pred\s+::")

    def test_math_does_not_copy_primitive_numeric_predicates(self):
        math = source("core/Math.mon")
        self.assertNotRegex(
            math,
            r"(?m)^define\s+(?:sign|zero\?|positive\?|negative\?|divisible\?)\s+::",
        )

    def test_sequence_composes_functor_and_foldable(self):
        functor = source("core/prelude/Data/Functor.mon")
        sequence = source("core/prelude/Coll.mon")

        self.assertRegex(functor, r"(?m)^\s*map\s+::")
        self.assertNotRegex(functor, r"(?m)^\s*fmap\s+::")
        self.assertRegex(sequence, r"(?m)^class\s+\(Functor c, Foldable c\)\s+=>\s+Sequence c where")

        class_body = sequence.split("class ", 1)[1].split("\n\ndefine ", 1)[0]
        self.assertNotRegex(class_body, r"(?m)^\s*(?:map|foldl|foldr)\s+::")
        self.assertRegex(sequence, r"(?m)^instance\s+Functor\s+Coll$")
        self.assertRegex(sequence, r"(?m)^instance\s+Foldable\s+Coll$")

    def test_public_names_do_not_hide_unrelated_abstractions(self):
        coll = source("core/prelude/Coll.mon")
        function = source("core/prelude/Function.mon")
        data_list = source("core/prelude/Data/List.mon")

        self.assertNotRegex(coll, r"(?m)^define\s+(?:append|both)\s+::")
        self.assertRegex(coll, r"(?m)^define\s+snoc\s+::")
        self.assertRegex(coll, r"(?m)^define\s+bothPredicates\s+::")
        self.assertNotRegex(function, r"(?m)^define\s+times\s+::")
        self.assertNotRegex(data_list, r"(?m)^define\s+length\s+::")


if __name__ == "__main__":
    unittest.main()
