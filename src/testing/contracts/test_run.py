import base64
import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def load_runner():
    from src.testing import runner
    return runner


class TestRunnerFormattingTests(unittest.TestCase):
    def test_unicode_glyphs_are_default_for_redirected_output(self):
        from src.tooling.console import Console

        with mock.patch.dict(os.environ, {"TERM": "dumb"}, clear=True):
            console = Console(color=False)
            self.assertEqual(console.glyph("✓", "OK"), "✓")

        with mock.patch.dict(os.environ, {"MONAD_GLYPHS": "ascii"}, clear=True):
            console = Console(color=False)
            self.assertEqual(console.glyph("✓", "OK"), "OK")

    def test_runner_accepts_an_explicit_glyph_policy(self):
        runner_mod = load_runner()
        self.assertEqual(runner_mod.parse_args(["--glyphs", "ascii"]).glyphs, "ascii")
        self.assertTrue(runner_mod.parse_args(["--profile"]).profile)

    def test_runner_accepts_parallel_jobs_and_auto_resolves_a_safe_default(self):
        runner_mod = load_runner()
        self.assertEqual(runner_mod.parse_args(["--jobs", "3"]).jobs, 3)
        self.assertGreaterEqual(runner_mod.resolve_test_jobs(0), 1)

    def test_case_environments_share_only_the_prewarmed_core_cache(self):
        runner_mod = load_runner()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            suite_env = {"HOME": str(root / "suite-home"),
                         "MONAD_CORE_CACHE": str(root / "core-cache")}
            first = runner_mod.case_environment(suite_env, root / "one")
            second = runner_mod.case_environment(suite_env, root / "two")

        self.assertNotEqual(first["HOME"], second["HOME"])
        self.assertEqual(first["MONAD_CORE_CACHE"], second["MONAD_CORE_CACHE"])
        self.assertTrue(first["HOME"].endswith("one/home"))

    def test_parallel_execution_renders_results_in_fixture_order(self):
        runner_mod = load_runner()
        cases = [
            runner_mod.TestCase(name="codegen.slow", metadata={"TEST-ID": "tests.codegen.slow"}),
            runner_mod.TestCase(name="codegen.fast", metadata={"TEST-ID": "tests.codegen.fast"}),
        ]
        options = runner_mod.RunnerOptions(
            filters=(), fail_fast=False, max_failures=None,
            keep_passing_artifacts=False, preserve_failures=False,
            no_color=True, jobs=2,
        )
        runner = runner_mod.Runner(cases, options)

        def finish_out_of_order(case, _tmpdir, **_kwargs):
            if case.name.endswith("slow"):
                time.sleep(0.03)
            return runner_mod.TestResult(name=case.name, passed=True, elapsed_ns=1,
                                         section="codegen")

        with tempfile.TemporaryDirectory() as td, mock.patch.object(
                runner, "run", side_effect=finish_out_of_order), mock.patch.object(
                runner_mod, "print_result"):
            runner_mod.execute_cases(runner, cases, Path(td), {}, jobs=2)

        self.assertEqual([result.name for result in runner.results], ["codegen.slow", "codegen.fast"])

    def test_profile_report_aggregates_compiler_and_fixture_phases(self):
        runner_mod = load_runner()
        samples = [
            {"case": "codegen.one", "kind": "compiler", "elapsed_ns": 80, "returncode": 0},
            {"case": "codegen.one", "kind": "fixture", "elapsed_ns": 20, "returncode": 0},
            {"case": "codegen.two", "kind": "compiler", "elapsed_ns": 50, "returncode": 0},
        ]

        report = runner_mod.build_profile_report(samples, suite_elapsed_ns=200)

        self.assertEqual(report["suite_elapsed_ns"], 200)
        self.assertEqual(report["totals"], {"compiler": {"count": 2, "elapsed_ns": 130},
                                             "fixture": {"count": 1, "elapsed_ns": 20}})
        self.assertEqual(report["cases"][0], {
            "name": "codegen.one", "compiler_ns": 80, "fixture_ns": 20,
            "subprocesses": 2,
        })

    def test_make_keeps_unicode_glyphs_when_term_is_dumb(self):
        env = {**os.environ, "TERM": "dumb", "NO_COLOR": "1"}
        rich = subprocess.run(
            [str(ROOT / "make"), "--no-color", "--glyphs", "unicode", "help"],
            cwd=ROOT, env=env, text=True, capture_output=True, check=False,
        )
        plain = subprocess.run(
            [str(ROOT / "make"), "--no-color", "--glyphs", "ascii", "help"],
            cwd=ROOT, env=env, text=True, capture_output=True, check=False,
        )
        self.assertEqual(rich.returncode, 0, rich.stderr)
        self.assertEqual(plain.returncode, 0, plain.stderr)
        self.assertIn("◆", rich.stdout)
        self.assertIn("─", rich.stdout)
        self.assertIn("*", plain.stdout)
        self.assertIn("-", plain.stdout)

    def test_native_link_prefers_lld_when_available(self):
        if shutil.which("ld.lld") is None:
            self.skipTest("ld.lld is not installed on this host")
        binary = Path(os.environ.get("MONAD_BINARY", ROOT / "build" / "bin" / "monad"))
        if not binary.exists():
            self.skipTest(f"compiler binary is unavailable: {binary}")

        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "Linker.mon"
            output = Path(td) / "linker"
            source.write_text("(module Main)\n(show 1)\n", encoding="utf-8")
            result = subprocess.run(
                [str(binary), "-v", str(source), "-o", str(output)],
                cwd=ROOT,
                env={**os.environ, "MONAD_LINKER": "lld"},
                text=True,
                capture_output=True,
                check=False,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[link] clang -fuse-ld=lld", result.stdout)

    def test_batch_compiler_compiles_tab_delimited_jobs_linearly(self):
        binary = Path(os.environ.get("MONAD_BINARY", ROOT / "build" / "bin" / "monad"))
        if not binary.exists():
            self.skipTest(f"compiler binary is unavailable: {binary}")

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            first = root / "First.mon"
            second = root / "Second.mon"
            first_out = root / "first"
            second_out = root / "second"
            first.write_text("(module Main)\n(show 11)\n", encoding="utf-8")
            second.write_text("(module Main)\n(show 22)\n", encoding="utf-8")
            protocol = f"{first}\t{first_out}\n{second}\t{second_out}\n"
            result = subprocess.run(
                [str(binary), "batch", "-q"],
                cwd=ROOT,
                input=protocol,
                env={**os.environ, "HOME": str(root / "home")},
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), [
                f"BATCH OK {first_out}", f"BATCH OK {second_out}",
            ])
            self.assertEqual(subprocess.run([str(first_out)], text=True,
                                            capture_output=True, check=False).stdout.strip(), "11")
            self.assertEqual(subprocess.run([str(second_out)], text=True,
                                            capture_output=True, check=False).stdout.strip(), "22")

    def test_batch_reuses_immutable_core_metadata_between_jobs(self):
        binary = Path(os.environ.get("MONAD_BINARY", ROOT / "build" / "bin" / "monad"))
        if not binary.exists():
            self.skipTest(f"compiler binary is unavailable: {binary}")

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            first = root / "First.mon"
            second = root / "Second.mon"
            first_out = root / "first"
            second_out = root / "second"
            first.write_text("(module Main)\n(show 31)\n", encoding="utf-8")
            second.write_text("(module Main)\n(show 32)\n", encoding="utf-8")
            protocol = f"{first}\t{first_out}\n{second}\t{second_out}\n"
            result = subprocess.run(
                [str(binary), "batch", "-q"], cwd=ROOT, input=protocol,
                env={**os.environ, "HOME": str(root / "home"),
                     "MONAD_CORE": str(ROOT / "core"),
                     "MONAD_CORE_CACHE": str(root / "core-cache"),
                     "MONAD_BATCH_TRACE_REUSE": "1"},
                text=True, capture_output=True, check=False,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("BATCH OK", result.stdout)
        self.assertGreaterEqual(result.stdout.count("[batch-reuse]"), 1)

    def test_batch_compiler_rejects_malformed_job_lines(self):
        binary = Path(os.environ.get("MONAD_BINARY", ROOT / "build" / "bin" / "monad"))
        if not binary.exists():
            self.skipTest(f"compiler binary is unavailable: {binary}")

        result = subprocess.run(
            [str(binary), "batch", "-q"], cwd=ROOT, input="not-a-job\n",
            text=True, capture_output=True, check=False,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("batch: line 1 must be <input.mon> TAB <output-path>", result.stderr)

    def test_runner_marks_plain_runtime_cases_for_batch_compilation(self):
        runner_mod = load_runner()
        plain = runner_mod.TestCase(
            name="codegen.batchable", metadata={"TEST-EXPECT": "run"},
        )
        reader = runner_mod.TestCase(
            name="reader.not-batchable", metadata={"TEST-EXPECT": "parse-json"},
        )

        self.assertTrue(runner_mod.batch_case_eligible(plain))
        self.assertFalse(runner_mod.batch_case_eligible(reader))

    def test_runner_routes_batchable_compile_requests_to_persistent_client(self):
        runner_mod = load_runner()
        calls = []

        class FakeBatchClient:
            def compile(self, args, home):
                calls.append((args, home))
                return runner_mod.CommandResult(args=args, returncode=0,
                                                stdout="", elapsed_ns=1)

        previous_enabled = getattr(runner_mod.BATCH_CONTEXT, "enabled", False)
        previous_env = getattr(runner_mod.CASE_CONTEXT, "env", None)
        runner_mod.BATCH_CONTEXT.enabled = True
        runner_mod.CASE_CONTEXT.env = {"HOME": "/tmp/private-home"}
        try:
            with mock.patch.object(runner_mod, "batch_client", return_value=FakeBatchClient()):
                result = runner_mod.run_monad(["sample.mon", "-o", "sample.out"])
        finally:
            runner_mod.BATCH_CONTEXT.enabled = previous_enabled
            if previous_env is None:
                del runner_mod.CASE_CONTEXT.env
            else:
                runner_mod.CASE_CONTEXT.env = previous_env

        self.assertEqual(result.returncode, 0)
        self.assertEqual(calls, [(["sample.mon", "-o", "sample.out"], "/tmp/private-home")])

    def test_batch_clients_recycle_before_unbounded_rss_growth(self):
        runner_mod = load_runner()
        self.assertFalse(runner_mod.batch_recycle_required(3, 500 * 1024 * 1024))
        self.assertTrue(runner_mod.batch_recycle_required(4, 500 * 1024 * 1024))
        self.assertTrue(runner_mod.batch_recycle_required(1, 512 * 1024 * 1024))

    def test_batch_client_recycles_a_real_compiler_process(self):
        runner_mod = load_runner()
        if not runner_mod.MONAD.exists():
            self.skipTest(f"compiler binary is unavailable: {runner_mod.MONAD}")

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "leak-probe.mon"
            source.write_text("(module Main)\n(show 1)\n", encoding="utf-8")
            env = {
                **os.environ,
                "HOME": str(root / "base-home"),
                "MONAD_CORE": str(ROOT / "core"),
                "MONAD_CORE_CACHE": str(root / "core-cache"),
            }
            previous_enabled = getattr(runner_mod.BATCH_CONTEXT, "enabled", False)
            previous_env = getattr(runner_mod.CASE_CONTEXT, "env", None)
            previous_test_env = runner_mod.MONAD_TEST_ENV
            runner_mod.BATCH_CONTEXT.enabled = True
            runner_mod.CASE_CONTEXT.env = {**env, "HOME": str(root / "case-home")}
            runner_mod.MONAD_TEST_ENV = env
            pids = []
            try:
                for index in range(5):
                    result = runner_mod.run_monad([
                        str(source), "-o", str(root / f"output-{index}"),
                    ])
                    self.assertEqual(result.returncode, 0, result.stdout)
                    pids.append(runner_mod.BATCH_CONTEXT.client.process.pid)
            finally:
                runner_mod.close_batch_clients()
                runner_mod.BATCH_CONTEXT.enabled = previous_enabled
                if previous_env is None:
                    del runner_mod.CASE_CONTEXT.env
                else:
                    runner_mod.CASE_CONTEXT.env = previous_env
                runner_mod.MONAD_TEST_ENV = previous_test_env

        self.assertGreater(len(set(pids)), 1)

    def test_progress_alignment_width_is_section_local(self):
        runner_mod = load_runner()
        short = runner_mod.TestCase(
            name="wisp.short",
            metadata={"TEST-ID": "tests.wisp.short"},
            origin=ROOT / "tests" / "wisp" / "a.mon",
            origin_line=1,
            origin_col=1,
        )
        long = runner_mod.TestCase(
            name="layout.long",
            metadata={"TEST-ID": "tests.codegen.rt-layout-long"},
            origin=ROOT / "tests" / "codegen" / "deeply_nested_layout_case_with_long_name.mon",
            origin_line=1,
            origin_col=1,
        )

        options = runner_mod.RunnerOptions(
            filters=(),
            fail_fast=False,
            max_failures=None,
            keep_passing_artifacts=False,
            preserve_failures=False,
            no_color=True,
        )
        runner = runner_mod.Runner([short, long], options)

        self.assertLess(
            runner.section_widths["wisp"].location,
            runner.section_widths["layout"].location,
        )


    def test_default_tier_filter_selects_regression_only(self):
        runner_mod = load_runner()
        regression = runner_mod.TestCase(
            name="language.green",
            metadata={"TEST-ID": "tests.language.green", "TEST-TIER": "regression"},
        )
        future = runner_mod.TestCase(
            name="language.future",
            metadata={"TEST-ID": "tests.language.future", "TEST-TIER": "future"},
        )
        args = runner_mod.parse_args([])

        selected = runner_mod.filter_cases([regression, future], args)

        self.assertEqual([case.name for case in selected], ["language.green"])

    def test_all_tiers_keeps_quarantined_tests_visible(self):
        runner_mod = load_runner()
        regression = runner_mod.TestCase(
            name="language.green",
            metadata={"TEST-ID": "tests.language.green", "TEST-TIER": "regression"},
        )
        known_fail = runner_mod.TestCase(
            name="language.todo",
            metadata={"TEST-ID": "tests.language.todo", "TEST-TIER": "known-fail"},
        )
        args = runner_mod.parse_args(["--all-tiers"])

        selected = runner_mod.filter_cases([regression, known_fail], args)

        self.assertEqual([case.name for case in selected], ["language.green", "language.todo"])

    def test_corpus_json_golden_table_detects_ast_mismatch(self):
        runner_mod = load_runner()
        with tempfile.TemporaryDirectory() as td:
            table = Path(td) / "atoms.golden.jsonl"
            table.write_text(
                json.dumps({"name": "one", "json": {"type": "number", "value": 1}}) + "\n",
                encoding="utf-8",
            )
            case = runner_mod.TestCase(
                name="language.one",
                metadata={
                    "TEST-EXPECT-JSON-TABLE": str(table),
                    "TEST-EXPECT-JSON-KEY": "one",
                },
            )

            self.assertIsNone(runner_mod.compare_corpus_json_golden(case, {"type": "number", "value": 1}))
            mismatch = runner_mod.compare_corpus_json_golden(case, {"type": "number", "value": 2})

        self.assertIn("AST JSON did not match", mismatch)

    def test_single_line_diagnostic_golden_ignores_terminal_newline(self):
        runner_mod = load_runner()
        case = runner_mod.TestCase(name="diagnostic.setbang", metadata={})
        with tempfile.TemporaryDirectory() as td:
            golden = Path(td) / "diagnostic.stdout"
            golden.write_text("set!\n", encoding="utf-8")

            mismatch = runner_mod.check_expected_diagnostics(
                case, "source.mon:1:1: error: 'set!' requires 2 arguments\n",
                golden,
            )

        self.assertIsNone(mismatch)

    def test_embedded_desugar_golden_requests_ast_trace(self):
        runner_mod = load_runner()
        expected = "(module Main [])\n"
        case = runner_mod.TestCase(
            name="reader.embedded-desugar",
            metadata={
                "TEST-EXPECT": "parse-json",
                "TEST-EXPECT-DESUGAR-B64": base64.b64encode(expected.encode()).decode(),
            },
            source="(module Main)\n",
        )

        calls = []

        def fake_run_monad(args):
            calls.append(args)
            output_base = Path(args[args.index("-o") + 1])
            Path(f"{output_base}.json").write_text("{}", encoding="utf-8")
            return runner_mod.CommandResult(
                args=args,
                returncode=0,
                stdout=f"=== desugared AST\n{expected}=== end desugared AST\n",
                elapsed_ns=0,
            )

        with tempfile.TemporaryDirectory() as td, mock.patch.object(runner_mod, "run_monad", side_effect=fake_run_monad):
            passed, message, _output = runner_mod.run_case(case, Path(td))

        self.assertTrue(passed, message)
        self.assertEqual(len(calls), 1)
        self.assertIn("--trace=ast", calls[0])

    def test_compile_only_case_skips_redundant_reader_emit(self):
        runner_mod = load_runner()
        case = runner_mod.TestCase(
            name="codegen.compile-only",
            metadata={"TEST-EXPECT": "compile"},
            source="(module Main)\n",
        )
        calls = []

        def fake_run_monad(args):
            calls.append(args)
            output = Path(args[args.index("-o") + 1])
            output.write_text("#!/bin/sh\n", encoding="utf-8")
            output.chmod(0o755)
            return runner_mod.CommandResult(args=args, returncode=0, stdout="", elapsed_ns=0)

        with tempfile.TemporaryDirectory() as td, mock.patch.object(
            runner_mod, "run_monad", side_effect=fake_run_monad
        ):
            passed, message, _output = runner_mod.run_case(case, Path(td))

        self.assertTrue(passed, message)
        self.assertEqual(len(calls), 1)
        self.assertNotIn("--emit-json", calls[0])


if __name__ == "__main__":
    unittest.main()
