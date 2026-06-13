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

        runner = runner_mod.Runner([short, long])

        self.assertLess(
            runner.section_widths["wisp"].location,
            runner.section_widths["layout"].location,
        )

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
