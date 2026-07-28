import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary, resolve_runtime_archive


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()
RUNTIME = resolve_runtime_archive(MONAD)


class HowToExampleTests(unittest.TestCase):
    EXAMPLES = (
        "how_to/Syntax.mon",
        "how_to/AlgebraicDataTypes.mon",
        "how_to/Macros.mon",
        "how_to/Iter.mon",
        "how_to/QuickCheck.mon",
        "how_to/ReaderSyntax.mon",
        "how_to/FirstOrderModalLogic.mon",
    )

    def test_monad_sources_use_multiline_haskell_style_data_declarations(self):
        offenders = []
        for root_name in ("core", "how_to", "tests"):
            for source in (ROOT / root_name).rglob("*.mon"):
                if source.name.startswith(".#"):
                    continue
                for line_number, line in enumerate(source.read_text().splitlines(), 1):
                    stripped = line.lstrip()
                    if stripped.startswith("data ") and (
                        " = " in stripped or " | " in stripped
                    ):
                        offenders.append(f"{source.relative_to(ROOT)}:{line_number}")
        self.assertEqual(offenders, [], "single-line data declarations: " + ", ".join(offenders))

    def test_wisp_scrutinee_clauses_desugar_to_match(self):
        with tempfile.TemporaryDirectory(prefix="monadc-scrutinee-clauses-") as td:
            temp = Path(td)
            source = temp / "ScrutineeClauses.mon"
            source.write_text(
                "module Main\n\n"
                "data BranchStatus\n"
                "  = OpenBranch\n"
                "  | ClosedBranch Int Int\n\n"
                "layout Tableau\n"
                "  [status -> BranchStatus]\n\n"
                "define tableau-closed? :: Tableau -> Bool\n"
                "  tableau.status\n"
                "    | [ClosedBranch _ _] -> True\n"
                "    | OpenBranch -> False\n\n"
                "show tableau-closed? (Tableau (ClosedBranch 2 7))\n"
                "show tableau-closed? (Tableau OpenBranch)\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            compile_result = subprocess.run(
                [str(MONAD), source.name, "-o", str(temp / "ScrutineeClauses")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run = subprocess.run(
                [str(temp / "ScrutineeClauses")], cwd=temp, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "True\nFalse\n")

    def test_multi_argument_pattern_clause_keeps_each_parameter_type(self):
        """Matching one argument must not give its ADT type to later arguments."""
        with tempfile.TemporaryDirectory(prefix="monadc-pmatch-parameter-types-") as td:
            temp = Path(td)
            source = temp / "PatternParameterTypes.mon"
            source.write_text(
                "module Main\n\n"
                "data Value\n"
                "  = Text String\n"
                "  | Count Int\n\n"
                "define select :: Value -> String -> String\n"
                "  [Text value] suffix -> value ++ suffix\n"
                "  [Count _] suffix -> suffix\n\n"
                "show (select (Count 1) \"b\")\n"
            )
            self._compile_and_expect_output(source, temp, "b\n")

    def test_match_branches_may_construct_different_variants_of_same_adt(self):
        """A match result is the ADT, not the first branch constructor telescope."""
        with tempfile.TemporaryDirectory(prefix="monadc-match-adt-result-") as td:
            temp = Path(td)
            source = temp / "MatchADTResult.mon"
            source.write_text(
                "module Main\n\n"
                "data Node\n"
                "  = Named String\n"
                "  | Paired Int Int\n\n"
                "define rebuild :: Node -> Node\n"
                "  node -> (match node with\n"
                "    | [Named name] -> (Named name)\n"
                "    | [Paired left right] -> (Paired left right))\n\n"
                "define paired? :: Node -> Bool\n"
                "  [Paired _ _] -> True\n"
                "  _ -> False\n\n"
                "show paired? (rebuild (Paired 20 22))\n"
            )
            self._compile_and_expect_output(source, temp, "True\n")

    def test_recursive_collection_match_keeps_fully_applied_result_type(self):
        """A collection match helper must return Bool, never its Pi telescope."""
        with tempfile.TemporaryDirectory(prefix="monadc-recursive-coll-match-") as td:
            temp = Path(td)
            source = temp / "RecursiveCollectionMatch.mon"
            source.write_text(
                "module Main\n\n"
                "data Formula\n"
                "  = Atom String\n"
                "  | Negation Formula\n\n"
                "define same? :: Formula -> Formula -> Bool\n"
                "  left right -> True\n\n"
                "define contains-formula? :: Formula -> [Formula] -> Bool\n"
                "  formula formulas -> (match formulas with\n"
                "    | [] -> False\n"
                "    | [candidate|rest] -> (if (same? formula candidate) True (contains-formula? formula rest)))\n\n"
                "define contradicts? :: Formula -> [Formula] -> Bool\n"
                "  formula rest -> (match formula with\n"
                "    | [Negation body] -> (contains-formula? body rest)\n"
                "    | _ -> (contains-formula? (Negation formula) rest))\n\n"
                "show (contradicts? (Negation (Atom \"P\")) [(Atom \"P\")])\n"
            )
            self._compile_and_expect_output(source, temp, "True\n")

    def test_exported_function_may_pattern_match_private_result_adt(self):
        """An exported API body must retain constructor metadata when imported."""
        with tempfile.TemporaryDirectory(prefix="monadc-exported-result-adt-") as td:
            temp = Path(td)
            (temp / "Decisions.mon").write_text(
                "module Decisions [Decision Valid Inconclusive decide accepted?]\n\n"
                "data BranchStatus\n"
                "  = OpenBranch\n"
                "  | ClosedBranch Int Int\n\n"
                "layout Tableau [entries :: [Int]] [status -> BranchStatus]\n\n"
                "data Decision\n"
                "  = Valid Tableau\n"
                "  | Inconclusive Tableau String\n\n"
                "data SearchOutcome\n"
                "  = SearchAccepted\n"
                "  | SearchRejected\n\n"
                "define from-search :: SearchOutcome -> Decision\n"
                "  [SearchAccepted] -> (Valid (Tableau [] (ClosedBranch 1 2)))\n"
                "  [SearchRejected] -> (Inconclusive (Tableau [] OpenBranch) \"no\")\n\n"
                "define decide :: Bool -> Decision\n"
                "  flag -> (from-search (if flag SearchAccepted SearchRejected))\n\n"
                "define accepted? :: Decision -> Bool\n"
                "  [Valid _] -> True\n"
                "  [Inconclusive _] -> False\n"
            )
            source = temp / "Main.mon"
            source.write_text(
                "import Decisions\n\nmodule Main\n\n"
                "show (accepted? (decide True))\n"
            )
            self._compile_and_expect_output(source, temp, "True\n")

    def test_public_modal_search_api_preserves_proof_result_abi(self):
        with tempfile.TemporaryDirectory(prefix="monadc-public-modal-search-") as td:
            temp = Path(td)
            source = temp / "PublicModalSearch.mon"
            shutil.copy2(ROOT / "tests/public_modal_search.mon", source)
            self._compile_and_expect_output(source, temp, "True\nTrue\nTrue\n")

    def _compile_and_expect_output(self, source, temp, expected):
        output = temp / source.stem
        env = os.environ.copy()
        env["HOME"] = str(temp / "home")
        Path(env["HOME"]).mkdir()
        env["MONAD_CORE"] = str(ROOT / "core")
        env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
        compiled = subprocess.run(
            [str(MONAD), source.name, "-o", str(output)],
            cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stdout[-6000:])
        run = subprocess.run(
            [str(output)], cwd=temp, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=False, timeout=30,
        )
        self.assertEqual(run.returncode, 0, run.stdout[-4000:])
        self.assertEqual(run.stdout, expected)

    def test_reader_declarations_use_mixfix_hole_patterns(self):
        offenders = []
        legacy_rule = re.compile(r"^\s+(?:prefix|postfix|infix|binder)\s+")
        for root_name in ("core", "how_to", "tests"):
            for source in (ROOT / root_name).rglob("*.mon"):
                if source.name.startswith(".#"):
                    continue
                for line_number, line in enumerate(source.read_text().splitlines(), 1):
                    if legacy_rule.match(line):
                        offenders.append(f"{source.relative_to(ROOT)}:{line_number}")
        self.assertEqual(offenders, [], "legacy reader rule declarations: " + ", ".join(offenders))

    def test_readme_listed_how_to_examples_compile(self):
        for example in self.EXAMPLES:
            with self.subTest(example=example):
                with tempfile.TemporaryDirectory(prefix="monadc-howto-home-") as home:
                    output = Path(home) / Path(example).stem
                    env = os.environ.copy()
                    env["HOME"] = home
                    env["MONAD_CORE"] = str(ROOT / "core")
                    env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
                    result = subprocess.run(
                        [str(MONAD), str(ROOT / example), "-o", str(output)],
                        cwd=ROOT,
                        env=env,
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        check=False,
                    )

                self.assertEqual(
                    result.returncode,
                    0,
                    msg=f"{example} failed with {MONAD}\n{result.stdout[-4000:]}",
                )

    def test_syntax_example_links_to_requested_output(self):
        with tempfile.TemporaryDirectory(prefix="monadc-syntax-example-") as td:
            temp = Path(td)
            output = temp / "Syntax"
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            result = subprocess.run(
                [str(MONAD), str(ROOT / "how_to/Syntax.mon"), "-o", str(output)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )

            executable = Path(str(output) + ".exe") if os.name == "nt" else output
            self.assertEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertTrue(executable.exists(), result.stdout[-4000:])

    def test_first_order_modal_logic_executes_across_core_module_boundary(self):
        """Imported recursive ADTs/layouts must preserve their runtime ABI."""
        source_text = (ROOT / "how_to/FirstOrderModalLogic.mon").read_text()
        self.assertIn("define barcan-math :: Formula\n  ∀x. □Fx -> □∀x. Fx",
                      source_text)
        self.assertIn(
            'define barcan-sexp :: Formula\n  (implies (∀ "x" (□ Fx)) (□ (∀ "x" Fx)))',
            source_text,
        )
        self.assertNotIn("render-formula (barcan-formula", source_text)
        with tempfile.TemporaryDirectory(prefix="monadc-modal-howto-") as td:
            temp = Path(td)
            source = temp / "FirstOrderModalLogic.mon"
            shutil.copy2(ROOT / "how_to/FirstOrderModalLogic.mon", source)
            output = temp / "FirstOrderModalLogic"
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)

            compile_result = subprocess.run(
                [str(MONAD), source.name],
                cwd=temp, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run_result = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
                timeout=30,
            )
            self.assertEqual(run_result.returncode, 0, run_result.stdout[-4000:])
            self.assertEqual(
                run_result.stdout,
                "∀x. □Fx -> □∀x. Fx\n"
                "∀x. □Fx -> □∀x. Fx\n"
                "1.  ¬(∀x. □Fx -> □∀x. Fx)   (w)\n"
                "2.  ∀x. □Fx                 (w)(1)\n"
                "3.  ¬□∀x. Fx                (w)(1)\n"
                "4.  wRv                     (3)\n"
                "5.  ¬∀x. Fx                 (v)(3)\n"
                "6.  ¬Fa                     (v)(5)\n"
                "7.  □Fa                     (w)(2)\n"
                "8.  Fa                      (v)(7,4)\n\n"
                "CLOSED: contradictory lines 6 and 8.\n",
            )

    def test_reader_dsl_tutorial_is_small_executable_and_dual_notation(self):
        source = ROOT / "how_to/ReaderSyntax.mon"
        source_text = source.read_text()
        self.assertIn("reader-syntax Expression", source_text)
        self.assertIn("data Expression\n  = Number     Int", source_text)
        self.assertIn("_⊕_  add-expression", source_text)
        self.assertIn("_⁻   negate-expression", source_text)
        self.assertIn("twice_ duplicate-expression", source_text)
        self.assertIn("define duplicate-expression :: Syntax -> Syntax", source_text)
        self.assertIn('(syntax-list (syntax-symbol "add-expression") value value)',
                      source_text)
        self.assertIn("define canonical :: Expression", source_text)
        self.assertIn("define surface :: Expression", source_text)
        with tempfile.TemporaryDirectory(prefix="monadc-reader-howto-") as td:
            temp = Path(td)
            output = temp / "ReaderSyntax"
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            compile_result = subprocess.run(
                [str(MONAD), str(source), "-o", str(output)], cwd=ROOT,
                env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "7\n7\n-7\n6\n")

    def test_formula_reader_is_expected_type_scoped(self):
        """Logic punctuation must not capture types, clauses, or for arrows."""
        with tempfile.TemporaryDirectory(prefix="monadc-reader-scope-") as td:
            temp = Path(td)
            output = temp / "reader-syntax-scope"
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            result = subprocess.run(
                [str(MONAD), str(ROOT / "tests/reader_syntax_scope.mon"),
                 "-o", str(output)],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout[-4000:])
            run = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(
                run.stdout,
                "∀x. □Fx -> □∀x. Fx\n"
                "¬(∀x. □Fx -> □∀x. Fx)\n"
                "6\n",
            )

    def test_reader_model_is_available_as_core_values(self):
        with tempfile.TemporaryDirectory(prefix="monadc-syntax-values-") as td:
            temp = Path(td)
            output = temp / "syntax-values"
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            compile_result = subprocess.run(
                [str(MONAD), str(ROOT / "tests/syntax_reader_values.mon"),
                 "-o", str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "3\nname\nExample\n_⊕_\n")

    def test_formula_reader_obeys_declared_precedence_and_associativity(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-precedence-") as td:
            temp = Path(td)
            output = temp / "reader-precedence"
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            env["MONAD_READER_DEBUG"] = "1"
            compile_result = subprocess.run(
                [str(MONAD), str(ROOT / "tests/reader_syntax_precedence.mon"),
                 "-o", str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            self.assertIn(
                "=> (implies (∨ (∧ (¬ Fx) Gx) Hx) (implies Fx Gx))",
                compile_result.stdout,
            )
            run = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            lines = run.stdout.splitlines()
            self.assertEqual(lines[0], lines[1])

    def test_conflicting_reader_rules_are_rejected_at_declaration(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-conflict-") as td:
            temp = Path(td)
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            result = subprocess.run(
                [str(MONAD), str(ROOT / "tests/reader_syntax_conflict.mon"),
                 "-o", str(temp / "conflict")], cwd=ROOT, env=env,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertIn("conflicting prefix rule '!' for reader Example",
                          result.stdout)
            self.assertIn("reader_syntax_conflict.mon:5:", result.stdout)

    def test_invalid_reader_rules_have_source_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-invalid-") as td:
            temp = Path(td)
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            result = subprocess.run(
                [str(MONAD), str(ROOT / "tests/reader_syntax_invalid_rule.mon"),
                 "-o", str(temp / "invalid")], cwd=ROOT, env=env,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertIn("invalid reader-syntax rule", result.stdout)
            self.assertIn("reader_syntax_invalid_rule.mon:4:", result.stdout)

    def test_reader_declarations_accept_crlf_source_files(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-crlf-") as td:
            temp = Path(td)
            source = temp / "ReaderCRLF.mon"
            output = temp / "reader-crlf"
            source.write_bytes(
                b"reader-syntax Expression\r\n"
                b"  _+_ add-expression 1 left\r\n\r\n"
                b"module Main\r\n\r\n"
                b"data Expression\r\n"
                b"  = Number Int\r\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)

            result = subprocess.run(
                [str(MONAD), str(source), "-o", str(output)],
                cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )

            self.assertEqual(result.returncode, 0, result.stdout[-4000:])

    def test_reader_rule_storage_grows_without_a_hidden_fixed_limit(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-capacity-") as td:
            temp = Path(td)
            output = temp / "reader-capacity"
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            compile_result = subprocess.run(
                [str(MONAD), str(ROOT / "tests/reader_syntax_dynamic_capacity.mon"),
                 "-o", str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "42\n")

    def test_reader_grammar_is_activated_by_importing_its_owner_module(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-module-") as td:
            temp = Path(td)
            home = temp / "home"
            home.mkdir()
            (temp / "TinyArithmetic.mon").write_text(
                "reader-syntax Expression\n"
                "  ~_ negate-expression\n"
                "  twice_ duplicate-expression\n"
                "  _⊕_ add-expression 1 left\n"
                "  _⊗_ multiply-expression 2 left\n\n"
                "module TinyArithmetic [Expression Number negate-expression "
                "add-expression multiply-expression evaluate duplicate-expression]\n\n"
                "data Expression\n"
                "  = Number     Int\n"
                "  | Negated    Expression\n"
                "  | Added      Expression Expression\n"
                "  | Multiplied Expression Expression\n\n"
                "define negate-expression :: Expression -> Expression\n"
                "  value -> Negated value\n\n"
                "define add-expression :: Expression -> Expression -> Expression\n"
                "  left right -> Added left right\n\n"
                "define multiply-expression :: Expression -> Expression -> Expression\n"
                "  left right -> Multiplied left right\n\n"
                "define duplicate-expression :: Syntax -> Syntax\n"
                "  value -> (add-expression value value)\n\n"
                "define evaluate :: Expression -> Int\n"
                "  [Number value] -> value\n"
                "  [Negated value] -> 0 - (evaluate value)\n"
                "  [Added left right] -> (evaluate left) + (evaluate right)\n"
                "  [Multiplied left right] -> (evaluate left) * (evaluate right)\n"
            )
            (temp / "Main.mon").write_text(
                "import TinyArithmetic\n\nmodule Main\n\n"
                "define one :: Expression (Number 1)\n"
                "define two :: Expression (Number 2)\n"
                "define three :: Expression (Number 3)\n\n"
                "define twicefold :: Expression (Number 9)\n\n"
                "define canonical :: Expression\n"
                "  (add-expression one (multiply-expression two three))\n\n"
                "define surface :: Expression\n"
                "  one ⊕ two ⊗ three\n\n"
                "define transformed :: Expression\n"
                "  twice one\n\n"
                "define whole-identifier :: Expression\n"
                "  twicefold\n\n"
                "show evaluate canonical\nshow evaluate surface\n"
                "show evaluate transformed\nshow evaluate whole-identifier\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            compile_result = subprocess.run(
                [str(MONAD), "Main.mon", "-o", str(temp / "Main")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run = subprocess.run(
                [str(temp / "Main")], cwd=temp, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "7\n7\n2\n9\n")

            cached_compile = subprocess.run(
                [str(MONAD), "Main.mon", "-o", str(temp / "CachedMain")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(cached_compile.returncode, 0,
                             cached_compile.stdout[-4000:])
            cached_run = subprocess.run(
                [str(temp / "CachedMain")], cwd=temp, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(cached_run.returncode, 0, cached_run.stdout[-4000:])
            self.assertEqual(cached_run.stdout, "7\n7\n2\n9\n")

            (temp / "WithoutImport.mon").write_text(
                "module Main\n\ndefine surface :: Expression\n  one ⊕ two\n"
            )
            without_import = subprocess.run(
                [str(MONAD), "WithoutImport.mon", "-o", str(temp / "WithoutImport")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertNotEqual(without_import.returncode, 0,
                                without_import.stdout[-4000:])

    def test_reader_grammar_does_not_leak_through_transitive_imports(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-transitive-") as td:
            temp = Path(td)
            home = temp / "home"
            home.mkdir()
            (temp / "ArithmeticReader.mon").write_text(
                "reader-syntax Expression\n"
                "  _⊕_ add-expression 1 left\n\n"
                "module ArithmeticReader\n"
            )
            (temp / "Arithmetic.mon").write_text(
                "import ArithmeticReader\n\n"
                "module Arithmetic [Expression Number Added add-expression]\n\n"
                "data Expression\n"
                "  = Number Int\n"
                "  | Added  Expression Expression\n\n"
                "define add-expression :: Expression -> Expression -> Expression\n"
                "  left right -> Added left right\n"
            )
            (temp / "Main.mon").write_text(
                "import Arithmetic\n\n"
                "module Main\n\n"
                "define one :: Expression (Number 1)\n"
                "define two :: Expression (Number 2)\n\n"
                "define leaked :: Expression\n"
                "  one ⊕ two\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            result = subprocess.run(
                [str(MONAD), "Main.mon", "-o", str(temp / "Main")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertFalse((temp / "Main").exists(), result.stdout[-4000:])

    def test_directly_imported_reader_owners_cannot_ambiguously_define_one_type(self):
        with tempfile.TemporaryDirectory(prefix="monadc-reader-ambiguity-") as td:
            temp = Path(td)
            home = temp / "home"
            home.mkdir()
            (temp / "LeftReader.mon").write_text(
                "reader-syntax Expression\n"
                "  _⊕_ add-left 1 left\n\n"
                "module LeftReader\n"
            )
            (temp / "RightReader.mon").write_text(
                "reader-syntax Expression\n"
                "  _⊗_ add-right 1 left\n\n"
                "module RightReader\n"
            )
            (temp / "Main.mon").write_text(
                "import LeftReader\n"
                "import RightReader\n\n"
                "module Main\n\n"
                "define ambiguous :: Expression\n"
                "  one ⊕ two\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            result = subprocess.run(
                [str(MONAD), "Main.mon", "-o", str(temp / "Main")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertIn("ambiguous reader-syntax Expression", result.stdout)
            self.assertIn("LeftReader.mon", result.stdout)
            self.assertIn("RightReader.mon", result.stdout)

    def test_syntax_transformers_execute_compile_time_control_and_arithmetic(self):
        """Syntax-returning functions compute during expansion, not at runtime."""
        with tempfile.TemporaryDirectory(prefix="monadc-syntax-evaluator-") as td:
            temp = Path(td)
            source = temp / "CompileTime.mon"
            source.write_text(
                "module Main\n\n"
                "define choose-syntax :: Syntax -> Syntax -> Syntax -> Syntax\n"
                "  condition yes no -> (if condition yes no)\n\n"
                "define add-syntax :: Syntax -> Syntax -> Syntax\n"
                "  left right -> (let [sum left] (+ sum right))\n\n"
                "(define answer :: Int\n"
                "  (choose-syntax (20 < 22) (add-syntax 20 22) missing-runtime-name))\n\n"
                "(show answer)\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            compile_result = subprocess.run(
                [str(MONAD), source.name, "-o", str(temp / "CompileTime")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run = subprocess.run(
                [str(temp / "CompileTime")], cwd=temp, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "42\n")

    def test_syntax_transformer_errors_are_source_located(self):
        with tempfile.TemporaryDirectory(prefix="monadc-syntax-error-") as td:
            temp = Path(td)
            source = temp / "BadTransformer.mon"
            source.write_text(
                "module Main\n\n"
                "define reject-syntax :: Syntax -> Syntax\n"
                "  value -> (syntax-error value \"rejected by transformer\")\n\n"
                "(define bad :: Int (reject-syntax 1))\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            result = subprocess.run(
                [str(MONAD), source.name, "-o", str(temp / "BadTransformer")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertRegex(result.stdout, r"BadTransformer\.mon:\d+:\d+: error:")
            self.assertIn("BadTransformer.mon:6:", result.stdout)
            self.assertIn("rejected by transformer", result.stdout)

    def test_syntax_transformers_inspect_trees_and_call_compile_time_helpers(self):
        with tempfile.TemporaryDirectory(prefix="monadc-syntax-inspection-") as td:
            temp = Path(td)
            source = temp / "InspectSyntax.mon"
            source.write_text(
                "module Main\n\n"
                "define list-form? :: Syntax -> Syntax\n"
                "  value -> (syntax-list? value)\n\n"
                "define third-form :: Syntax -> Syntax\n"
                "  value -> (if (list-form? value) (syntax-list-ref value 2) 0)\n\n"
                "define named-answer? :: Syntax -> Syntax\n"
                "  value -> (if (== (syntax-symbol-text value) \"answer\") 1 0)\n\n"
                "define source-line-of :: Syntax -> Syntax\n"
                "  value -> (syntax-source-line value)\n\n"
                "define fresh-names-distinct :: Syntax -> Syntax\n"
                "  ignored -> (let [a (syntax-gensym \"tmp\")] "
                "[b (syntax-gensym \"tmp\")] "
                "(if (!= (syntax-symbol-text a) (syntax-symbol-text b)) 1 0))\n\n"
                "define classify-syntax :: Syntax -> Syntax\n"
                "  value -> (match (syntax-kind value) with "
                "| \"number\" -> 1 | \"symbol\" -> 2 | _ -> 0)\n\n"
                "define context-round-trip :: Syntax -> Syntax\n"
                "  ignored -> (let [scope (syntax-fresh-context \"local\")] "
                "(let [identifier (syntax-add-context (syntax-symbol \"x\") scope)] "
                "(if (== (syntax-context identifier) scope) "
                "(if (== (syntax-symbol-text (syntax-remove-context identifier)) \"x\") 1 0) 0)))\n\n"
                "(define answer :: Int (third-form (unbound-head 11 42)))\n"
                "(define name-check :: Int (named-answer? answer))\n"
                "(define source-line :: Int (source-line-of 99))\n"
                "(define hygiene-check :: Int (fresh-names-distinct ignored))\n"
                "(define kind-check :: Int (classify-syntax 123))\n"
                "(define context-check :: Int (context-round-trip ignored))\n"
                "(show answer)\n"
                "(show name-check)\n"
                "(show source-line)\n"
                "(show hygiene-check)\n"
                "(show kind-check)\n"
                "(show context-check)\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            env["MONAD_MACRO_TRACE"] = "1"
            compile_result = subprocess.run(
                [str(MONAD), source.name, "-o", str(temp / "InspectSyntax")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            trace = [
                json.loads(line.removeprefix("[macro-trace] "))
                for line in compile_result.stdout.splitlines()
                if line.startswith("[macro-trace] ")
            ]
            self.assertTrue(trace, compile_result.stdout[-4000:])
            self.assertIn("third-form", {event["macro"] for event in trace})
            self.assertTrue(all("input" in event and "output" in event
                                for event in trace))
            run = subprocess.run(
                [str(temp / "InspectSyntax")], cwd=temp, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "42\n1\n26\n1\n1\n1\n")

    def test_syntax_transformers_evaluate_quasiquote_holes(self):
        with tempfile.TemporaryDirectory(prefix="monadc-syntax-quasiquote-") as td:
            temp = Path(td)
            source = temp / "QuasiquoteSyntax.mon"
            source.write_text(
                "module Main\n\n"
                "define double-syntax :: Syntax -> Syntax\n"
                "  value -> (quasiquote (+ (unquote value) (unquote value)))\n\n"
                "define splice-add :: Syntax -> Syntax\n"
                "  values -> (quasiquote (+ (unquote-splicing values)))\n\n"
                "(define answer :: Int (double-syntax 21))\n"
                "(define spliced :: Int (splice-add (syntax-list 20 22)))\n"
                "(show answer)\n"
                "(show spliced)\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            compile_result = subprocess.run(
                [str(MONAD), source.name, "-o", str(temp / "QuasiquoteSyntax")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run = subprocess.run(
                [str(temp / "QuasiquoteSyntax")], cwd=temp, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "42\n42\n")

    def test_for_syntax_imports_transformers_without_runtime_exports(self):
        with tempfile.TemporaryDirectory(prefix="monadc-for-syntax-") as td:
            temp = Path(td)
            (temp / "SyntaxTools.mon").write_text(
                "module SyntaxTools [forty-two runtime-secret]\n\n"
                "define forty-two :: Syntax -> Syntax\n"
                "  ignored -> 42\n\n"
                "define runtime-secret :: Int 99\n"
            )
            (temp / "Main.mon").write_text(
                "import for-syntax SyntaxTools\n\n"
                "module Main\n\n"
                "(define answer :: Int (forty-two ignored))\n"
                "(show answer)\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            compile_result = subprocess.run(
                [str(MONAD), "Main.mon", "-o", str(temp / "Main")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run = subprocess.run(
                [str(temp / "Main")], cwd=temp, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "42\n")

            (temp / "RuntimeLeak.mon").write_text(
                "import for-syntax SyntaxTools\n\n"
                "module Main\n\n(show runtime-secret)\n"
            )
            leak = subprocess.run(
                [str(MONAD), "RuntimeLeak.mon", "-o", str(temp / "RuntimeLeak")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertNotEqual(leak.returncode, 0, leak.stdout[-4000:])
            self.assertIn("runtime-secret", leak.stdout)

    def test_syntax_transformers_do_not_leak_through_transitive_imports(self):
        with tempfile.TemporaryDirectory(prefix="monadc-transformer-transitive-") as td:
            temp = Path(td)
            home = temp / "home"
            home.mkdir()
            (temp / "SyntaxTools.mon").write_text(
                "module SyntaxTools [forty-two]\n\n"
                "define forty-two :: Syntax -> Syntax\n"
                "  ignored -> (syntax-number 42)\n"
            )
            (temp / "Bridge.mon").write_text(
                "import for-syntax SyntaxTools\n\n"
                "module Bridge\n"
            )
            (temp / "Main.mon").write_text(
                "import Bridge\n\n"
                "module Main\n\n"
                "define answer :: Int (forty-two 0)\n"
                "show answer\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            result = subprocess.run(
                [str(MONAD), "Main.mon", "-o", str(temp / "Main")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertFalse((temp / "Main").exists(), result.stdout[-4000:])

    def test_direct_for_syntax_imports_reject_ambiguous_transformers(self):
        with tempfile.TemporaryDirectory(prefix="monadc-transformer-ambiguity-") as td:
            temp = Path(td)
            home = temp / "home"
            home.mkdir()
            for module, value in (("LeftTools", 1), ("RightTools", 2)):
                (temp / f"{module}.mon").write_text(
                    f"module {module} [answer]\n\n"
                    "define answer :: Syntax -> Syntax\n"
                    f"  ignored -> (syntax-number {value})\n"
                )
            (temp / "Main.mon").write_text(
                "import for-syntax LeftTools\n"
                "import for-syntax RightTools\n\n"
                "module Main\n\n"
                "define value :: Int (answer 0)\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            result = subprocess.run(
                [str(MONAD), "Main.mon", "-o", str(temp / "Main")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertIn("ambiguous Syntax transformer 'answer'", result.stdout)
            self.assertIn("LeftTools.mon", result.stdout)
            self.assertIn("RightTools.mon", result.stdout)

    def test_private_syntax_transformers_are_not_importable(self):
        with tempfile.TemporaryDirectory(prefix="monadc-transformer-private-") as td:
            temp = Path(td)
            home = temp / "home"
            home.mkdir()
            (temp / "SyntaxTools.mon").write_text(
                "module SyntaxTools [public-answer]\n\n"
                "define public-answer :: Syntax -> Syntax\n"
                "  ignored -> (syntax-number 42)\n\n"
                "define private-answer :: Syntax -> Syntax\n"
                "  ignored -> (syntax-number 99)\n"
            )
            (temp / "Main.mon").write_text(
                "import for-syntax SyntaxTools\n\n"
                "module Main\n\n"
                "define value :: Int (private-answer 0)\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            result = subprocess.run(
                [str(MONAD), "Main.mon", "-o", str(temp / "Main")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertIn("private-answer", result.stdout)

    def test_exported_transformers_can_call_private_lexical_helpers(self):
        with tempfile.TemporaryDirectory(prefix="monadc-transformer-helper-") as td:
            temp = Path(td)
            home = temp / "home"
            home.mkdir()
            (temp / "SyntaxTools.mon").write_text(
                "module SyntaxTools [public-answer]\n\n"
                "define private-answer :: Syntax -> Syntax\n"
                "  ignored -> (syntax-number 42)\n\n"
                "define public-answer :: Syntax -> Syntax\n"
                "  value -> (private-answer value)\n"
            )
            (temp / "Main.mon").write_text(
                "import for-syntax SyntaxTools\n\n"
                "module Main\n\n"
                "define value :: Int (public-answer 0)\n"
                "show value\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            compile_result = subprocess.run(
                [str(MONAD), "Main.mon", "-o", str(temp / "Main")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run = subprocess.run(
                [str(temp / "Main")], cwd=temp, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "42\n")

    def test_reader_dsl_runs_from_an_installed_read_only_core(self):
        with tempfile.TemporaryDirectory(prefix="monadc-installed-reader-") as td:
            temp = Path(td)
            prefix = temp / "prefix"
            bin_dir = prefix / "bin"
            lib_dir = prefix / "lib"
            installed_core = lib_dir / "monad" / "core"
            work = temp / "work"
            home = temp / "home"
            bin_dir.mkdir(parents=True)
            lib_dir.mkdir(parents=True)
            work.mkdir()
            home.mkdir()
            installed_monad = bin_dir / ("monad.exe" if os.name == "nt" else "monad")
            shutil.copy2(MONAD, installed_monad)
            shutil.copytree(ROOT / "core", installed_core)
            shutil.copy2(ROOT / "how_to/ReaderSyntax.mon", work / "ReaderSyntax.mon")
            shutil.copy2(RUNTIME, lib_dir / "libmonad.a")
            if os.name != "nt":
                for path in installed_core.rglob("*"):
                    path.chmod(0o555 if path.is_dir() else 0o444)
                installed_core.chmod(0o555)
            env = os.environ.copy()
            env["HOME"] = str(home)
            env.pop("MONAD_CORE", None)
            env.pop("MONAD_RUNTIME_LIB", None)
            compile_result = subprocess.run(
                [str(installed_monad), "ReaderSyntax.mon"], cwd=work, env=env,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run = subprocess.run(
                [str(work / "ReaderSyntax")], cwd=work, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "7\n7\n-7\n6\n")

    def test_recursive_compile_time_helpers_process_syntax_lists(self):
        with tempfile.TemporaryDirectory(prefix="monadc-recursive-syntax-") as td:
            temp = Path(td)
            source = temp / "RecursiveSyntax.mon"
            source.write_text(
                "module Main\n\n"
                "define sum-syntax :: Syntax -> Syntax\n"
                "  values -> (if (== (syntax-list-length values) 0) 0 "
                "(+ (syntax-list-ref values 0) "
                "(sum-syntax (syntax-list-tail values))))\n\n"
                "(define answer :: Int (sum-syntax (syntax-list 10 20 12)))\n"
                "(show answer)\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            compile_result = subprocess.run(
                [str(MONAD), source.name, "-o", str(temp / "RecursiveSyntax")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run = subprocess.run(
                [str(temp / "RecursiveSyntax")], cwd=temp, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "42\n")

    def test_compile_time_array_syntax_is_constructible_and_inspectable(self):
        with tempfile.TemporaryDirectory(prefix="monadc-array-syntax-") as td:
            temp = Path(td)
            source = temp / "ArraySyntax.mon"
            source.write_text(
                "module Main\n\n"
                "define second-array-item :: Syntax -> Syntax\n"
                "  values -> (if (syntax-array? values) "
                "(syntax-array-ref values 1) 0)\n\n"
                "(define answer :: Int "
                "(second-array-item (syntax-array 11 42)))\n"
                "(show answer)\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            compile_result = subprocess.run(
                [str(MONAD), source.name, "-o", str(temp / "ArraySyntax")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout[-4000:])
            run = subprocess.run(
                [str(temp / "ArraySyntax")], cwd=temp, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout[-4000:])
            self.assertEqual(run.stdout, "42\n")

    def test_recursive_syntax_transformer_stops_at_deterministic_budget(self):
        with tempfile.TemporaryDirectory(prefix="monadc-runaway-syntax-") as td:
            temp = Path(td)
            source = temp / "RunawaySyntax.mon"
            source.write_text(
                "module Main\n\n"
                "define forever :: Syntax -> Syntax\n"
                "  value -> (forever value)\n\n"
                "(define answer :: Int (forever 1))\n"
            )
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            result = subprocess.run(
                [str(MONAD), source.name, "-o", str(temp / "RunawaySyntax")],
                cwd=temp, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False, timeout=30,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertGreater(result.returncode, 0, result.stdout[-4000:])
            self.assertIn("deterministic evaluation limit", result.stdout)
            self.assertIn("RunawaySyntax.mon:", result.stdout)

    def test_quickcheck_compiles_and_runs_from_a_read_only_installed_core(self):
        with tempfile.TemporaryDirectory(prefix="monadc-installed-howto-") as td:
            temp = Path(td)
            prefix = temp / "prefix"
            bin_dir = prefix / "bin"
            lib_dir = prefix / "lib"
            installed_core = lib_dir / "monad" / "core"
            work = temp / "work"
            home = temp / "home"
            bin_dir.mkdir(parents=True)
            lib_dir.mkdir(parents=True, exist_ok=True)
            work.mkdir()
            home.mkdir()

            installed_monad = bin_dir / ("monad.exe" if os.name == "nt" else "monad")
            shutil.copy2(MONAD, installed_monad)
            shutil.copytree(ROOT / "core", installed_core)
            shutil.copy2(ROOT / "how_to/QuickCheck.mon", work / "QuickCheck.mon")
            runtime_name = "libmonad.a"
            shutil.copy2(RUNTIME, lib_dir / runtime_name)

            if os.name != "nt":
                for path in installed_core.rglob("*"):
                    path.chmod(0o555 if path.is_dir() else 0o444)
                installed_core.chmod(0o555)

            env = os.environ.copy()
            env["HOME"] = str(home)
            env.pop("MONAD_CORE", None)
            env.pop("MONAD_RUNTIME_LIB", None)
            compile_result = subprocess.run(
                [str(installed_monad), "--test", "QuickCheck.mon"],
                cwd=work,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=30,
            )
            self.assertEqual(
                compile_result.returncode, 0, compile_result.stdout[-4000:]
            )

            executable = (
                work / "QuickCheck.exe" if os.name == "nt"
                else work / "QuickCheck"
            )
            run_result = subprocess.run(
                [str(executable)],
                cwd=work,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=30,
            )
            self.assertEqual(run_result.returncode, 0, run_result.stdout[-4000:])
            self.assertIn("QuickCheck\x1b[0m  Eq Bool", run_result.stdout)
            self.assertIn("QuickCheck\x1b[0m  Additive Int", run_result.stdout)

            law_result = subprocess.run(
                [str(installed_monad), "test", "QuickCheck.mon"],
                cwd=work,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=30,
            )
            self.assertEqual(law_result.returncode, 0, law_result.stdout[-4000:])
            self.assertIn("QuickCheck\x1b[0m  Eq Bool", law_result.stdout)
            self.assertIn("QuickCheck\x1b[0m  Additive Int", law_result.stdout)


if __name__ == "__main__":
    unittest.main()
