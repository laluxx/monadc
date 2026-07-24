import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary


MONAD = resolve_monad_binary()
ROOT = Path(__file__).resolve().parents[1]


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def clean_output(value: str) -> str:
    return ANSI_RE.sub("", value).strip()


class ReplTests(unittest.TestCase):
    def run_repl(
        self, source: str, timeout: int = 15
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory(prefix="monadc-repl-") as td:
            env = os.environ.copy()
            env["HOME"] = td
            env["MONAD_NO_PROMPT"] = "1"
            return subprocess.run(
                [str(MONAD), "repl"],
                input=source,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=timeout,
            )

    def test_repl_evaluates_piped_one_line_wisp(self):
        result = self.run_repl("show 42\n")

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(clean_output(result.stdout), "42")

    def test_repl_bare_integer_expression_prints_one_line(self):
        result = self.run_repl("3 + 3\n")

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(clean_output(result.stdout), "6")

    def test_repl_applies_core_identity_function_in_wisp_syntax(self):
        result = self.run_repl("id 3\n")

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(clean_output(result.stdout), "3")

    def test_repl_prints_builtin_features_without_crashing(self):
        result = self.run_repl("show *features*\n")

        self.assertEqual(result.returncode, 0, result.stdout)
        output = clean_output(result.stdout)
        self.assertNotIn("runtime crash", output)
        self.assertNotIn("SIGSEGV", output)
        self.assertNotIn("JIT session error", output)
        self.assertRegex(output, r"[\[(].*:[A-Za-z0-9_-]+.*[\])]")

    def test_repl_builtin_features_remain_valid_across_many_jit_modules(self):
        result = self.run_repl(("show *features*\nshow 1\n" * 40))

        self.assertEqual(result.returncode, 0, result.stdout)
        output = clean_output(result.stdout)
        self.assertNotIn("runtime crash", output)
        self.assertNotIn("IR verification failed", output)
        self.assertEqual(output.splitlines().count("1"), 40)

    def test_repl_recovers_after_a_codegen_error_in_the_same_process(self):
        result = self.run_repl("show missing-name\nshow 42\n")

        self.assertEqual(result.returncode, 0, result.stdout)
        output = clean_output(result.stdout)
        self.assertIn("unbound variable: missing-name", output)
        self.assertEqual(output.splitlines()[-1], "42")
        self.assertNotIn("runtime crash", output)

    def test_repl_command_protocol_is_machine_readable(self):
        result = self.run_repl(",help\n,complete sho\n")

        self.assertEqual(result.returncode, 0, result.stdout)
        output = clean_output(result.stdout)
        self.assertIn("REPL commands:", output)
        self.assertIn("__COMPLETIONS__", output)
        self.assertIn("show\tbuiltin", output)
        self.assertTrue(output.endswith("__END__"), output)

    def test_repl_persists_heap_values_across_jit_modules(self):
        result = self.run_repl(
            "define values (list 1 2 3)\n"
            "show 0\n"
            "show values\n"
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        output = clean_output(result.stdout)
        self.assertNotIn("runtime crash", output)
        self.assertEqual(output.splitlines()[-1], "(1 2 3)")

    def test_repl_handles_a_long_lived_session_without_jit_corruption(self):
        source = "".join(f"show ({index} + 1)\n" for index in range(150))
        result = self.run_repl(source)

        self.assertEqual(result.returncode, 0, result.stdout)
        output = clean_output(result.stdout)
        self.assertNotIn("runtime crash", output)
        self.assertNotIn("IR verification failed", output)
        self.assertEqual(output.splitlines(), [str(index + 1) for index in range(150)])

    def test_repl_persists_top_level_value_definitions(self):
        result = self.run_repl(
            "define x 30\n"
            "x\n"
            "show (x + 12)\n"
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        lines = clean_output(result.stdout).splitlines()
        self.assertNotIn("runtime crash", result.stdout)
        self.assertNotIn("JIT session error", result.stdout)
        self.assertNotIn("ORC lookup failed", result.stdout)
        self.assertEqual(lines[0], "30")
        self.assertEqual(lines[-1], "42")

    def test_repl_evaluates_multiline_wisp_typed_function_then_call(self):
        result = self.run_repl(
            "define add2 :: Int -> Int\n"
            "  x -> x + 2\n"
            "\n"
            "show (add2 40)\n"
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("42", clean_output(result.stdout).splitlines())

    def test_repl_evaluates_general_indented_wisp_application(self):
        result = self.run_repl(
            "show\n"
            "  id 42\n"
            "\n"
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(clean_output(result.stdout), "42")

    def test_repl_evaluates_multiline_parenthesized_expression_at_eof(self):
        result = self.run_repl(
            "show (\n"
            "  id 42)\n"
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(clean_output(result.stdout), "42")

    def test_repl_evaluates_multiline_wisp_pattern_clauses(self):
        result = self.run_repl(
            "define choose :: Int -> Int\n"
            "  0 -> 10\n"
            "  x -> x + 1\n"
            "\n"
            "show (choose 0)\n"
            "show (choose 4)\n"
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(clean_output(result.stdout).splitlines(), ["10", "5"])

    def test_repl_reports_invalid_input_then_recovers_for_the_next_form(self):
        result = self.run_repl("]\nid 9\n")

        self.assertEqual(result.returncode, 0, result.stdout)
        output = clean_output(result.stdout)
        self.assertIn("error:", output.lower())
        self.assertNotIn("DEBUG", output)
        self.assertEqual(output.splitlines()[-1], "9")

    def test_repl_imported_core_method_runs_without_debug_noise(self):
        with tempfile.TemporaryDirectory(prefix="monadc-repl-import-") as td:
            env = os.environ.copy()
            env["HOME"] = td
            env["MONAD_NO_PROMPT"] = "1"
            result = subprocess.run(
                [str(MONAD), "repl"],
                input="import Data.Bool\nnot True\n",
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=15,
            )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("DEBUG nm:", result.stdout)
        self.assertNotIn("[dep] Warning:", result.stdout)
        self.assertNotIn("Class:", result.stdout)
        self.assertTrue(clean_output(result.stdout).endswith("False"), result.stdout)

    def test_eval_runs_import_then_expression_in_one_source_argument(self):
        with tempfile.TemporaryDirectory(prefix="monadc-eval-import-") as td:
            env = os.environ.copy()
            env["HOME"] = td
            result = subprocess.run(
                [str(MONAD), "eval", "import Data.Bool\nnot True"],
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=15,
            )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(clean_output(result.stdout).endswith("False"), result.stdout)

    def test_parallel_eval_imports_use_independent_shared_modules(self):
        with tempfile.TemporaryDirectory(prefix="monadc-eval-parallel-") as td:
            processes = []
            for index in range(6):
                home = os.path.join(td, str(index))
                os.mkdir(home)
                env = os.environ.copy()
                env["HOME"] = home
                processes.append(subprocess.Popen(
                    [str(MONAD), "eval", "import Data.Bool\nnot True"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                ))

            results = [process.communicate(timeout=20) for process in processes]

        for process, (output, _) in zip(processes, results):
            self.assertEqual(process.returncode, 0, output)
            self.assertTrue(clean_output(output).endswith("False"), output)

    def test_eval_data_list_import_hides_codegen_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="monadc-eval-list-") as td:
            env = os.environ.copy()
            env["HOME"] = td
            result = subprocess.run(
                [str(MONAD), "eval", "import Data.List\n1"],
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=20,
            )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("MONO [", result.stdout)
        self.assertNotIn("Constructor:", result.stdout)
        self.assertNotIn("Data type:", result.stdout)
        self.assertTrue(clean_output(result.stdout).endswith("1"), result.stdout)

    def test_repl_imports_every_core_module_in_one_live_session(self):
        module_names = []
        module_pattern = re.compile(
            r"^module[ \t]+([A-Za-z_][A-Za-z0-9_.]*)", re.MULTILINE
        )
        for path in sorted((ROOT / "core").rglob("*.mon")):
            if any(part.startswith(".") for part in path.relative_to(ROOT / "core").parts):
                continue
            match = module_pattern.search(path.read_text(encoding="utf-8"))
            if match:
                module_names.append(match.group(1))

        self.assertGreater(len(module_names), 20)
        source = "".join(f"import {name}\n" for name in module_names)
        source += "show 4242\n"
        result = self.run_repl(source, timeout=180)

        self.assertEqual(result.returncode, 0, result.stdout)
        output = clean_output(result.stdout)
        self.assertNotIn("runtime crash", output)
        self.assertNotIn("failed to compile module", output)
        self.assertNotIn("IR verification failed", output)
        self.assertNotIn("error:", output.lower())
        self.assertEqual(output.splitlines()[-1], "4242")


if __name__ == "__main__":
    unittest.main()
