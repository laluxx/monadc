import re
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def source(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class CoreAbstractionOwnershipTests(unittest.TestCase):
    def test_install_invalidates_compiled_core_abi_cache(self):
        makefile = source("Makefile")

        self.assertIn("CORE_CACHE_DIR ?= $(HOME)/.cache/monad/core", makefile)
        self.assertIn('rm -rf "$(CORE_CACHE_DIR)"', makefile)

    def test_core_declares_primitive_bootstrap_in_module_metadata(self):
        main_c = source("main.c")
        makefile = source("Makefile")
        cmake = source("CMakeLists.txt")

        self.assertNotIn("k_primitive_type_stems", main_c)
        self.assertNotIn("k_primitive_type_stems2", main_c)
        self.assertNotIn("Primitive.modules", main_c)
        self.assertIn("core_primitive_module_stems", main_c)
        self.assertIn(":bootstrap primitive", main_c)
        for relative in (
            "core/prelude/Data/Int.mon", "core/prelude/Data/Float.mon",
            "core/prelude/Data/Bool.mon", "core/prelude/Data/String.mon",
            "core/prelude/Data/Map.mon", "core/prelude/Data/Char.mon",
            "core/prelude/Data/Semigroup.mon", "core/prelude/Sequence.mon",
        ):
            self.assertIn(":bootstrap primitive", source(relative), relative)
        self.assertNotIn('*.modules', makefile)
        self.assertNotIn('*.modules', cmake)

    def test_empty_core_needs_no_primitive_manifest(self):
        with tempfile.TemporaryDirectory(prefix="monadc-empty-core-") as td:
            root = Path(td)
            (root / "core").mkdir()
            (root / "home").mkdir()
            fixture = root / "Main.mon"
            output = root / "Main"
            fixture.write_text("module Main []\nshow 42\n", encoding="utf-8")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(root / "core")
            env["HOME"] = str(root / "home")
            result = subprocess.run(
                [str(ROOT / "monad"), str(fixture), "-o", str(output)],
                cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertNotIn("Primitive.modules", result.stdout)

    def test_module_metadata_bootstraps_core_owned_primitive_method(self):
        with tempfile.TemporaryDirectory(prefix="monadc-primitive-core-") as td:
            root = Path(td)
            data = root / "core" / "prelude" / "Data"
            data.mkdir(parents=True)
            (root / "home").mkdir()
            for name in ("Bool.mon", "Int.mon"):
                (data / name).write_text(
                    source(f"core/prelude/Data/{name}"), encoding="utf-8")
            fixture = root / "Main.mon"
            output = root / "Main"
            fixture.write_text(
                "module Main []\nshow (positive? 1)\n", encoding="utf-8")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(root / "core")
            env["HOME"] = str(root / "home")
            result = subprocess.run(
                [str(ROOT / "monad"), str(fixture), "-o", str(output)],
                cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            run = subprocess.run(
                [str(output)], cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertEqual(run.stdout, "True\n")

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

    def test_bool_uses_real_module_metadata_and_commentary(self):
        bool_core = source("core/prelude/Data/Bool.mon")

        self.assertRegex(bool_core, r'(?m)^:author\s+"Laluxx"$')
        self.assertRegex(bool_core, r'(?m)^:version\s+"[^"]+"$')
        self.assertRegex(bool_core, r'(?m)^:keywords\s+"[^"]+"$')
        self.assertNotRegex(bool_core, r"(?mi)^;;\s*(?:Author|Version|Keywords):")
        self.assertIn(";;; Commentary:", bool_core)
        self.assertIn("finite set {True, False}", bool_core)

    def test_refactored_core_modules_use_real_metadata_and_commentary(self):
        modules = (
            "core/prelude/Control/Applicative.mon",
            "core/prelude/Control/Category.mon",
            "core/prelude/Control/Monad.mon",
            "core/prelude/Data/Bool.mon",
            "core/prelude/Data/Either.mon",
            "core/prelude/Data/Eq.mon",
            "core/prelude/Data/Functor.mon",
            "core/prelude/Data/Maybe.mon",
            "core/prelude/Data/Ord.mon",
            "core/prelude/Data/Profunctor.mon",
            "core/prelude/Data/Relation.mon",
            "core/prelude/Data/Semigroup.mon",
            "core/prelude/Numeric.mon",
            "core/prelude/Text/LineEditor.mon",
        )

        for module in modules:
            text = source(module)
            with self.subTest(module=module):
                self.assertRegex(text, r'(?m)^:author\s+(?:"[^"]+"|\S+)$')
                self.assertRegex(text, r'(?m)^:version\s+(?:"[^"]+"|\S+)$')
                self.assertRegex(text, r'(?m)^:keywords\s+(?:"[^"]+"|.+)$')
                self.assertNotRegex(text, r"(?mi)^;;\s*(?:Author|Version|Keywords):")
                self.assertIn(";;; Commentary:", text)

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

    def test_bifunctor_derives_one_sided_maps_from_bimap(self):
        functor_core = source("core/prelude/Data/Functor.mon")
        either_core = source("core/prelude/Data/Either.mon")
        bifunctor = functor_core.split("class Bifunctor p where", 1)[1]
        either_instance = either_core.split("instance Bifunctor Either", 1)[1].split(
            "\n\nmethod ", 1
        )[0]

        self.assertIn("first-map f value ->", bifunctor)
        self.assertIn("second-map g value ->", bifunctor)
        self.assertNotRegex(either_instance, r"(?m)^\s+first-map\s+")
        self.assertNotRegex(either_instance, r"(?m)^\s+second-map\s+")

    def test_applicative_and_monad_derive_operations_from_smaller_bases(self):
        applicative = source("core/prelude/Control/Applicative.mon")
        monad = source("core/prelude/Control/Monad.mon")
        maybe = source("core/prelude/Data/Maybe.mon")
        applicative_class = applicative.split("class Functor f => Applicative f where", 1)[1].split(
            "\n\nclass Applicative f => Alternative", 1
        )[0]
        monad_class = monad.split("class Applicative m => Monad m where", 1)[1].split(
            "\n\nclass Monad m => MonadPlus", 1
        )[0]
        maybe_applicative = maybe.split("instance Applicative Maybe", 1)[1].split(
            "\n\ninstance Monad Maybe", 1
        )[0]
        maybe_monad = maybe.split("instance Monad Maybe", 1)[1].split(
            "\n\nmethod ", 1
        )[0]

        for name in ("liftA", "liftA2", "liftA3"):
            self.assertRegex(applicative_class, rf"(?m)^\s+{name}\s+.*->")
            self.assertNotRegex(maybe_applicative, rf"(?m)^\s+{name}\s+")

        for name in ("return", "join", "then"):
            self.assertRegex(monad_class, rf"(?m)^\s+{name}\s+.*->")
            self.assertNotRegex(maybe_monad, rf"(?m)^\s+{name}\s+")

    def test_category_and_profunctor_derive_directional_conveniences(self):
        category = source("core/prelude/Control/Category.mon")
        profunctor = source("core/prelude/Data/Profunctor.mon")

        self.assertRegex(category, r"(?m)^\s+pipeCategory\s+f\s+g\s+->")
        self.assertIn("composeCategory g f", category)
        self.assertRegex(profunctor, r"(?m)^\s+lmap\s+f\s+value\s+->")
        self.assertRegex(profunctor, r"(?m)^\s+rmap\s+g\s+value\s+->")
        self.assertIn("dimap f (lambda (x) x) value", profunctor)
        self.assertIn("dimap (lambda (x) x) g value", profunctor)

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

    def test_unicode_semantics_are_versioned_and_entirely_core_owned(self):
        unicode_core = source("core/Text/Unicode.mon")
        infer_c = source("infer.c")
        dep_c = source("dep.c")
        codegen_c = source("codegen.c")
        runtime_c = source("runtime.c")
        runtime_h = source("runtime.h")

        self.assertIn('define unicode-version :: String', unicode_core)
        for name in (
            "byte-count", "valid-scalar?", "valid-utf8?", "decode-utf8",
            "scalar-count", "grapheme-count", "display-width", "text-width",
        ):
            self.assertRegex(unicode_core, rf"(?m)^define\s+{re.escape(name)}\s+::")
        self.assertIn("tests\n", unicode_core)
        self.assertNotIn("__rt_", unicode_core)
        self.assertRegex(unicode_core, r"byte \| >= 0 and <= 0x7f -> True")
        self.assertRegex(unicode_core, r"scalar \| >= 0x1100  and <= 0x115f")
        self.assertNotRegex(unicode_core, r"\(and\s+\(>=\s+(?:byte|scalar)\b")

        for compiler_source in (infer_c, dep_c, codegen_c):
            self.assertNotIn("__rt_utf8_width", compiler_source)
        self.assertNotIn("rt_utf8_width", runtime_c)
        self.assertNotIn("rt_utf8_width", runtime_h)

    def test_configuration_protocol_is_typed_and_core_owned(self):
        configuration = source("core/Configuration.mon")

        self.assertIn("module Configuration", configuration)
        self.assertIn("data ConfigValidation a", configuration)
        self.assertIn("layout ConfigDiagnostic", configuration)
        self.assertIn("data ConfigurationSource", configuration)
        self.assertIn("define require :: ConfigPath -> String -> Bool -> [ConfigDiagnostic]", configuration)
        self.assertNotIn("__rt_", configuration)
        self.assertNotIn("Map String", configuration)
        self.assertIn("ordinary Monad modules", configuration)

    def test_sequence_public_abi_has_class_and_concrete_coll_methods(self):
        coll_core = source("core/prelude/Sequence.mon")
        for name in (
            "filter", "prepend", "null?", "length", "reverse", "at", "nth",
            "take", "drop", "takeWhile", "dropWhile",
            "any?", "all?", "zip", "zipWith", "snoc",
        ):
            self.assertRegex(coll_core, rf"(?m)^\s+{re.escape(name)}\s+::")
            self.assertNotRegex(coll_core, rf"(?m)^define\s+{re.escape(name)}\s+::")

        concrete_methods = {
            "filter": "filter", "prepend": "prepend", "null?": "null?",
            "length": "length", "reverse": "reverse", "at": "at",
            "nth": "nth", "take": "take", "drop": "drop",
            "takeWhile": "take-while", "dropWhile": "drop-while",
            "any?": "any?", "all?": "all?", "zip": "zip",
            "zipWith": "zip-with", "snoc": "snoc",
        }
        for implementation in concrete_methods.values():
            self.assertRegex(
                coll_core,
                rf"(?m)^method\s+{re.escape(implementation)}\s+::",
            )
        self.assertIn("method concat :: Semigroup (c a) => c a -> c a -> c a", coll_core)
        self.assertIn("xs ys -> append xs ys", coll_core)

        self.assertNotRegex(coll_core, r"(?m)^(?:method|define)\s+append\s+::")

        implementation = coll_core.split("\ntests\n", 1)[0]
        self.assertNotIn(" ++ ", implementation)
        self.assertIn("import Data.Semigroup", coll_core)
        self.assertIn("xs ys -> append xs ys", implementation)
        self.assertIn("x xs -> __rt_prepend x xs", implementation)
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
        readline_core = source("core/prelude/Text/LineEditor.mon")

        self.assertIn("import Sequence", enum_core)
        self.assertNotIn(" ++ ", enum_core.split("\ntests\n", 1)[0])
        self.assertIn("prepend start", enum_core)

        self.assertIn("import Sequence", readline_core)
        self.assertNotRegex(readline_core, r"(?m)^define\s+append-list\s+::")
        self.assertNotRegex(
            readline_core,
            r"(?m)^define\s+(?:length-list|take-list|drop-list)\s+::",
        )
        self.assertNotIn(" ++ ", readline_core)
        self.assertIn("-> concat (take text cursor)", readline_core)
        self.assertIn("-> drop text cursor", readline_core)

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

    def test_semigroup_and_monoid_derive_nonminimal_operations(self):
        semigroup_core = source("core/prelude/Data/Semigroup.mon")
        class_source, instances = semigroup_core.split("instance Semigroup Int", 1)

        self.assertIn("sconcat []", class_source)
        self.assertIn("sconcat [x|xs]", class_source)
        self.assertIn("mappend x y", class_source)
        self.assertIn("mconcat []", class_source)
        self.assertIn("mconcat [x|xs]", class_source)
        self.assertNotRegex(instances, r"(?m)^\s+sconcat\s+")
        self.assertNotRegex(instances, r"(?m)^\s+mappend\s+")
        self.assertNotRegex(instances, r"(?m)^\s+mconcat\s+")

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
        registry_lookup = types_c.index("if (type_nominal_is_registered(name))")
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

    def test_numeric_instances_inherit_derived_num_and_integral_operations(self):
        numeric = source("core/prelude/Numeric.mon")
        num_class = numeric.split("class Num a where", 1)[1].split(
            "\n\nclass Num a => Integral", 1
        )[0]
        integral_class = numeric.split("class Num a => Integral a where", 1)[1].split(
            "\n\nclass Num a => Fractional", 1
        )[0]
        instances = numeric.split("instance Num Int", 1)[1].split("\n\ntests", 1)[0]

        for name in ("inc", "dec", "double", "square", "cube"):
            self.assertRegex(num_class, rf"(?m)^\s+{re.escape(name)}\b.*->")
            self.assertNotRegex(instances, rf"(?m)^\s+\(?{re.escape(name)}\b")

        for name in ("quotRem", "divMod", "even?", "odd?", "gcd", "lcm"):
            self.assertRegex(integral_class, rf"(?m)^\s+{re.escape(name)}\s+.*->")
            self.assertNotRegex(instances, rf"(?m)^\s+\(?{re.escape(name)}(?:\s|\))")

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

    def test_eq_and_ord_instances_inherit_derived_relations(self):
        eq_source = source("core/prelude/Data/Eq.mon")
        ord_source = source("core/prelude/Data/Ord.mon")
        eq_class, eq_instances = eq_source.split("instance Eq Int", 1)
        ord_class, ord_instances = ord_source.split("instance Ord Int", 1)

        self.assertRegex(eq_class, r"(?m)^\s+not-eq\?\s+.*->")
        self.assertNotRegex(eq_instances, r"(?m)^\s+\(?not-eq\?(?:\s|\))")

        for name in ("gt?", "gte?"):
            self.assertRegex(ord_class, rf"(?m)^\s+{re.escape(name)}\s+.*->")
            self.assertNotRegex(
                ord_instances,
                rf"(?m)^\s+\(?{re.escape(name)}(?:\s|\))",
            )

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

    def test_sequence_keeps_shape_orthogonal_to_functor_and_foldable(self):
        functor = source("core/prelude/Data/Functor.mon")
        foldable = source("core/prelude/Data/Foldable.mon")
        sequence = source("core/prelude/Sequence.mon")

        self.assertRegex(functor, r"(?m)^\s*map\s+::")
        self.assertNotRegex(functor, r"(?m)^\s*fmap\s+::")
        self.assertRegex(
            sequence,
            r"(?m)^class\s+Evaluation \(Strategy s\)\s+=>\s+Sequence s where",
        )
        class_body = sequence.split("class Evaluation", 1)[1].split("\n\ndefine ", 1)[0]
        self.assertRegex(class_body, r"(?m)^\s+type Element s$")
        self.assertRegex(class_body, r"(?m)^\s+type Strategy s$")
        self.assertRegex(class_body, r"(?m)^\s+uncons\s+::")
        self.assertRegex(class_body, r"(?m)^\s+lower\s+::")
        self.assertNotRegex(class_body, r"(?m)^\s*(?:map|foldl|foldr)\s+::")
        self.assertRegex(functor, r"(?m)^instance\s+Functor\s+Coll$")
        self.assertIn("class Foldable", foldable)
        self.assertNotRegex(sequence, r"(?m)^instance\s+(?:Functor|Foldable)\s+Coll$")

    def test_public_names_do_not_hide_unrelated_abstractions(self):
        coll = source("core/prelude/Sequence.mon")
        function = source("core/prelude/Function.mon")
        data_list = source("core/prelude/Data/List.mon")

        self.assertNotRegex(coll, r"(?m)^define\s+(?:append|both)\s+::")
        self.assertRegex(coll, r"(?m)^\s+snoc\s+::")
        self.assertNotIn("bothPredicates", coll)
        self.assertNotRegex(function, r"(?m)^define\s+times\s+::")
        self.assertNotRegex(data_list, r"(?m)^define\s+length\s+::")

    def test_set_membership_does_not_require_enumeration(self):
        data_set = source("core/prelude/Data/Set.mon")
        codegen = source("codegen.c")
        membership = data_set.split("class Membership s where", 1)[1].split(
            "\n\ninstance Membership", 1
        )[0]

        self.assertRegex(membership, r"(?m)^\s*membership\?\s+::")
        self.assertNotRegex(membership, r"(?m)^\s*(?:count|foldl|foldr|elements)\s+::")
        self.assertRegex(data_set, r"(?m)^instance Membership Set$")
        self.assertIn("define set-member? :: Eq a => a -> {a} -> Bool", data_set)
        self.assertNotIn("__rt_", data_set)
        self.assertNotIn('env_insert_builtin(ctx->env, "contains?"', codegen)

    def test_core_does_not_bypass_sequence_and_set_abstractions(self):
        set_core = source("core/prelude/Data/Set.mon")
        list_core = source("core/prelude/Data/List.mon")
        readline_core = source("core/prelude/Text/LineEditor.mon")

        self.assertIn("instance Eq a => Eq {a}", set_core)
        self.assertIn("data Set a", set_core)
        self.assertIn("SetValue [a]", set_core)
        self.assertNotIn("__rt_set_", set_core)
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

        self.assertNotIn("import Data.Eq", data_string)
        self.assertRegex(data_string, r"(?m)^method\s+startsWith\?\s+::")
        self.assertRegex(data_string, r"(?m)^method\s+endsWith\?\s+::")
        self.assertRegex(data_string, r"(?m)^method\s+includes\?\s+::")

    def test_sequence_has_one_coalgebra_and_one_producer_bridge(self):
        sequence = source("core/prelude/Sequence.mon")

        self.assertNotRegex(sequence, r"\bcoll-[A-Za-z0-9?!-]+")
        self.assertEqual(
            sequence.count("class Evaluation (Strategy s) => Sequence s where"),
            1,
        )
        self.assertIn("define sequence-transition :: Sequence s =>", sequence)
        self.assertIn("lower  :: s -> Producer", sequence)

    def test_data_modules_do_not_reintroduce_parallel_evaluation_shapes(self):
        forbidden = ("LazySequence", "StrictSequence", "LazyTree", "StrictTree")
        for path in sorted((ROOT / "core/prelude/Data").rglob("*.mon")):
            text = path.read_text(encoding="utf-8")
            for name in forbidden:
                self.assertNotRegex(text, rf"(?m)^class\s+{name}\b", str(path))


if __name__ == "__main__":
    unittest.main()
