import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def load_runner():
    spec = importlib.util.spec_from_file_location("test_runner", ROOT / "tests" / "run.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class TestRunnerFormattingTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
