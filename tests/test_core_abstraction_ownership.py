import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def source(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class CoreAbstractionOwnershipTests(unittest.TestCase):
    def test_core_declares_its_primitive_module_manifest(self):
        main_c = source("main.c")
        manifest = source("core/prelude/Data/Primitive.modules")
        makefile = source("Makefile")
        cmake = source("CMakeLists.txt")

        self.assertNotIn("k_primitive_type_stems", main_c)
        self.assertNotIn("k_primitive_type_stems2", main_c)
        self.assertIn("core_primitive_module_stems", main_c)
        self.assertIn('"%s/prelude/%s.mon", core_dir, stem', main_c)
        self.assertEqual(
            [line for line in manifest.splitlines() if line and not line.startswith(";")],
            ["Int", "Float", "Bool", "String", "Map", "Char", "Semigroup", "Sequence"],
        )
        self.assertIn('-o -name "*.modules"', makefile)
        self.assertIn('PATTERN "*.modules"', cmake)

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
        self.assertNotIn(
            'if (name && strcmp(name, "Bool") == 0) return type_bool();',
            types_c,
            "the compiler must not turn the core finite-set declaration back into TYPE_BOOL",
        )

    def test_bool_behavior_is_owned_by_core_methods(self):
        bool_core = source("core/prelude/Data/Bool.mon")
        for name in ("bool", "not?", "and?", "or?", "xor?", "implies?", "iff?"):
            self.assertRegex(bool_core, rf"(?m)^method\s+{re.escape(name)}\s+::")
            self.assertNotRegex(bool_core, rf"(?m)^define\s+{re.escape(name)}\s+::")

    def test_sum_type_behavior_is_owned_by_core_methods(self):
        modules = {
            "core/prelude/Data/Maybe.mon": (
                "maybe", "fromMaybe", "fromJust", "just?", "nothing?",
                "transform", "flatMap", "flatten", "apply", "replace",
                "combine", "combineThree", "toList", "listToMaybe", "orElse",
                "firstJust", "safeHead", "safeTail",
            ),
            "core/prelude/Data/Either.mon": (
                "either", "left?", "right?", "fromLeft", "fromRight",
                "mapLeft", "mapRight", "flatMap", "mapBoth",
            ),
        }
        for module, names in modules.items():
            text = source(module)
            for name in names:
                self.assertRegex(text, rf"(?m)^method\s+{re.escape(name)}\s+::")
                self.assertNotRegex(text, rf"(?m)^define\s+{re.escape(name)}\s+::")

    def test_string_behavior_is_owned_by_core_methods(self):
        string_core = source("core/prelude/Data/String.mon")
        self.assertRegex(
            string_core,
            r"(?m)^\(?type\s+String\s+\{\s*chars\s+∈\s+\[Char\]",
        )
        for name in ("startsWith?", "endsWith?", "includes?", "copy"):
            self.assertRegex(string_core, rf"(?m)^method\s+{re.escape(name)}\s+::")
            self.assertNotRegex(string_core, rf"(?m)^define\s+{re.escape(name)}\s+::")

        self.assertNotRegex(string_core, r"(?m)^method\s+(?:concat|append)\s+::")

        for obsolete in (
            "string-prefix?", "string-suffix?", "string-contains?",
            "string-copy", "string-append",
        ):
            self.assertNotIn(obsolete, string_core)

    def test_sequence_structure_is_owned_by_sequence_class(self):
        coll_core = source("core/prelude/Sequence.mon")
        for name in (
            "filter", "prepend", "concat", "null?", "length", "reverse", "at", "nth",
            "take", "drop", "takeWhile", "dropWhile",
            "any?", "all?", "zip", "zipWith", "snoc",
        ):
            self.assertRegex(coll_core, rf"(?m)^\s+{re.escape(name)}\s+::")
            self.assertNotRegex(coll_core, rf"(?m)^method\s+{re.escape(name)}\s+::")
            self.assertNotRegex(coll_core, rf"(?m)^define\s+{re.escape(name)}\s+::")

        self.assertNotRegex(coll_core, r"(?m)^(?:method|define)\s+append\s+::")

        implementation = coll_core.split("\ntests\n", 1)[0]
        self.assertNotIn(" ++ ", implementation)
        self.assertIn("import Data.Semigroup", coll_core)
        self.assertIn("concat xs ys      -> rt_coll_concat xs ys", implementation)
        self.assertIn("prepend x xs      -> __rt_prepend x xs", implementation)
        self.assertRegex(coll_core, r"(?m)^method head :: \[a\] -> a$")
        self.assertRegex(coll_core, r"(?m)^\s+:alias hd$")
        self.assertRegex(coll_core, r"(?m)^method tail :: \[a\] -> \[a\]$")
        self.assertRegex(coll_core, r"(?m)^\s+:alias tl$")
        self.assertRegex(coll_core, r"(?m)^method count :: \[a\] -> Int$")
        self.assertRegex(coll_core, r"(?m)^method empty\? :: \[a\] -> Bool$")
        self.assertRegex(coll_core, r"(?m)^\s+:alias is-empty\?$")
        self.assertNotRegex(coll_core, r"(?m)^method hd ::")
        self.assertNotRegex(coll_core, r"(?m)^method tl ::")
        infer = source("infer.c")
        codegen = source("codegen.c")
        for name in ("head", "tail", "count", "empty?"):
            self.assertNotIn(f'infer_env_insert(ctx->env, "{name}"', infer)
            self.assertNotIn(f'env_insert_builtin(ctx->env, "{name}"', codegen)
            self.assertNotIn(f'strcmp(head->symbol, "{name}") == 0', codegen)

    def test_data_list_does_not_duplicate_sequence_structure(self):
        list_core = source("core/prelude/Data/List.mon")
        for obsolete in (
            "list-drop", "list-take", "list-append", "list-reverse",
        ):
            self.assertNotIn(obsolete, list_core)

        for name in ("caar", "cadr", "cdar", "cddr", "caddr", "cdddr"):
            self.assertRegex(list_core, rf"(?m)^method\s+{name}\s+::")
            self.assertNotRegex(list_core, rf"(?m)^define\s+{name}\s+::")

    def test_core_clients_use_sequence_construction_methods(self):
        enum_core = source("core/prelude/Data/Enum.mon")
        readline_core = source("core/prelude/Text/Readline.mon")

        self.assertIn("import Sequence", enum_core)
        self.assertNotIn(" ++ ", enum_core.split("\ntests\n", 1)[0])
        self.assertIn("prepend start", enum_core)

        self.assertIn("import Sequence", readline_core)
        self.assertNotRegex(readline_core, r"(?m)^define\s+append-list\s+::")
        self.assertNotIn(" ++ ", readline_core)
        self.assertIn("-> concat (take-list", readline_core)

    def test_semigroup_owns_same_shaped_collection_append(self):
        semigroup_core = source("core/prelude/Data/Semigroup.mon")
        reader = source("reader.c")
        infer = source("infer.c")
        codegen = source("codegen.c")

        self.assertIn("instance Semigroup Coll", semigroup_core)
        self.assertIn("append xs ys -> __rt_concat xs ys", semigroup_core)
        self.assertIn("instance Monoid Coll", semigroup_core)
        self.assertRegex(semigroup_core, r"(?m)^\s+mempty\s+-> list$")
        self.assertNotIn("sconcat xs) => head xs", semigroup_core)
        self.assertIn('strcmp(name, "++") == 0 ? "append" : name', reader)
        self.assertNotIn('infer_env_insert(ctx->env, "++"', infer)
        self.assertNotIn('strcmp(head->symbol, "++") == 0', infer)
        self.assertNotIn('env_insert_builtin(ctx->env, "++"', codegen)
        self.assertNotIn('strcmp(head->symbol, "++") == 0', codegen)

        dependent_checker = source("dep.c")
        self.assertIn(
            'dep_env_declare(env, "__rt_concat", dep_eval(poly_poly_poly, ee, NULL));',
            dependent_checker,
        )
        for primitive in ("rt_coll_drop", "rt_coll_empty", "rt_coll_is_empty"):
            self.assertIn(
                f'dep_env_declare(env, "{primitive}"',
                dependent_checker,
            )

    def test_char_methods_are_typed_by_the_char_module(self):
        char_core = source("core/prelude/Data/Char.mon")
        predicates = re.findall(
            r"(?m)^method\s+([^\s]+\?)\s+::\s+([^\n]+)$",
            char_core,
        )

        self.assertGreater(len(predicates), 25)
        for name, signature in predicates:
            self.assertEqual(signature, "Self -> Bool", name)

        self.assertRegex(char_core, r"(?m)^method\s+ord\s+::\s+Self -> Int$")
        self.assertRegex(char_core, r"(?m)^method\s+chr\s+::\s+Int -> Char$")
        for name in ("upcase", "downcase", "toggle-case"):
            self.assertRegex(char_core, rf"(?m)^method\s+{name}\s+::\s+Self -> Char$")
        for name in ("digit->int", "hex-digit->int", "base36-digit->int"):
            self.assertRegex(char_core, rf"(?m)^method\s+{name}\s+::\s+Self -> Int$")
        for name in ("int->digit", "int->hex-digit", "int->base36-digit"):
            self.assertRegex(char_core, rf"(?m)^method\s+{name}\s+::\s+Int -> Char$")

    def test_registered_core_types_override_legacy_representation_fallbacks(self):
        types_c = source("types.c")
        registry_lookup = types_c.index("// Check alias registry")
        builtin_fallback = types_c.index("// Built-in types first")

        self.assertLess(
            registry_lookup,
            builtin_fallback,
            "a core type registration must win over a compiler representation fallback",
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
        sequence = source("core/prelude/Sequence.mon")

        self.assertRegex(functor, r"(?m)^\s*map\s+::")
        self.assertNotRegex(functor, r"(?m)^\s*fmap\s+::")
        self.assertRegex(sequence, r"(?m)^class\s+\(Functor c, Foldable c\)\s+=>\s+Sequence c where")

        class_body = sequence.split("class ", 1)[1].split("\n\ndefine ", 1)[0]
        self.assertNotRegex(class_body, r"(?m)^\s*(?:map|foldl|foldr)\s+::")
        self.assertRegex(sequence, r"(?m)^instance\s+Functor\s+Coll$")
        self.assertRegex(sequence, r"(?m)^instance\s+Foldable\s+Coll$")

    def test_public_names_do_not_hide_unrelated_abstractions(self):
        coll = source("core/prelude/Sequence.mon")
        function = source("core/prelude/Function.mon")
        data_list = source("core/prelude/Data/List.mon")

        self.assertNotRegex(coll, r"(?m)^define\s+(?:append|both)\s+::")
        self.assertRegex(coll, r"(?m)^\s+snoc\s+::")
        self.assertRegex(coll, r"(?m)^define\s+bothPredicates\s+::")
        self.assertNotRegex(function, r"(?m)^define\s+times\s+::")
        self.assertNotRegex(data_list, r"(?m)^define\s+length\s+::")

    def test_set_membership_does_not_require_enumeration(self):
        data_set = source("core/prelude/Data/Set.mon")
        codegen = source("codegen.c")
        membership = data_set.split("class Membership s where", 1)[1].split(
            "\n\ninstance Membership", 1
        )[0]

        self.assertRegex(membership, r"(?m)^\s*member\?\s+::")
        self.assertNotRegex(membership, r"(?m)^\s*(?:count|foldl|foldr|elements)\s+::")
        self.assertRegex(data_set, r"(?m)^instance Membership Set$")
        self.assertIn("(member? x s) => __rt_contains? s x", data_set)
        self.assertNotIn('env_insert_builtin(ctx->env, "contains?"', codegen)

    def test_core_does_not_bypass_sequence_and_set_abstractions(self):
        set_core = source("core/prelude/Data/Set.mon")
        list_core = source("core/prelude/Data/List.mon")
        readline_core = source("core/prelude/Text/Readline.mon")

        self.assertNotRegex(set_core, r"\b(?:head|empty\?|count)\b")
        self.assertIn("instance Eq Set", set_core)
        self.assertIn("x -> __rt_set_singleton x", set_core)
        self.assertIn("bool __rt_set_singleton", source("runtime.c"))
        self.assertNotRegex(list_core, r"\b(?:head|tail)\b")
        self.assertNotIn("count text", readline_core)
        self.assertIn("length text", readline_core)

    def test_map_module_owns_typed_map_operations(self):
        map_core = source("core/prelude/Data/Map.mon")

        for signature in (
            "member? :: Map k v -> k -> Bool",
            "keys :: Map k v -> [k]",
            "values :: Map k v -> [v]",
            "insert :: Map k v -> k -> v -> Map k v",
            "delete :: Map k v -> k -> Map k v",
            "merge :: Map k v -> Map k v -> Map k v",
        ):
            self.assertIn(signature, map_core)
        self.assertNotRegex(map_core, r"(?m)^define\s+(?:member\?|keys|values|insert|delete|merge)\s+::")
        self.assertIn("__rt_map_", map_core)

    def test_map_representation_preserves_core_key_and_value_types(self):
        types_h = source("types.h")
        types_c = source("types.c")
        infer_c = source("infer.c")

        self.assertIn("struct Type *map_key_type;", types_h)
        self.assertIn("struct Type *map_value_type;", types_h)
        self.assertIn("Type *type_map_of(Type *key_type, Type *value_type);", types_h)
        self.assertIn("Type *type_map_of(Type *key_type, Type *value_type)", types_c)
        self.assertIn("result = type_map_of(key_t, val_t);", infer_c)
        self.assertNotIn("result = type_map();\n        break;\n    }\n\n    case AST_ARRAY", infer_c)
        self.assertIn(
            'infer_env_insert(ctx->env, "__rt_map_keys", infer_generalise(ctx,\n'
            '        type_arrow(keys_map, keys_result), ctx->env));',
            infer_c,
        )
        self.assertIn("keys_result->element_type = type_clone(keys_fresh);", infer_c)

    def test_concrete_map_api_is_owned_by_core(self):
        data_map = source("core/prelude/Data/Map.mon")
        codegen = source("codegen.c")

        for name in ("member?", "keys", "values", "insert", "delete"):
            self.assertRegex(data_map, rf"(?m)^method\s+{re.escape(name)}\s+::")
        self.assertIn("Map k v", data_map)
        for public in ("assoc", "assoc!", "dissoc", "dissoc!", "find", "keys", "vals", "merge"):
            self.assertNotIn(f'env_insert_builtin(ctx->env, "{public}"', codegen)
            self.assertNotIn(f'strcmp(head->symbol, "{public}")', codegen)
        for private in (
            "__rt_map_assoc", "__rt_map_assoc!", "__rt_map_dissoc",
            "__rt_map_dissoc!", "__rt_map_find", "__rt_map_keys",
            "__rt_map_values", "__rt_map_merge",
        ):
            self.assertIn(private, codegen)

    def test_string_queries_are_composed_from_core_abstractions(self):
        data_string = source("core/prelude/Data/String.mon")
        codegen = source("codegen.c")

        self.assertNotRegex(
            data_string,
            r"\b(?:starts-with\?|ends-with\?|contains\?)\b",
        )
        for builtin in ("starts-with?", "ends-with?", "contains?"):
            self.assertNotIn(f'env_insert_builtin(ctx->env, "{builtin}"', codegen)
            self.assertNotIn(f'strcmp(head->symbol, "{builtin}")', codegen)

        self.assertNotIn("import Data.Eq", data_string)
        self.assertRegex(data_string, r"(?m)^method\s+startsWith\?\s+::")
        self.assertRegex(data_string, r"(?m)^method\s+endsWith\?\s+::")
        self.assertRegex(data_string, r"(?m)^method\s+includes\?\s+::")


if __name__ == "__main__":
    unittest.main()
