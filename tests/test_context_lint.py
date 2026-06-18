#!/usr/bin/env python3
"""Tests for the context garden linter (context/tools/context_lint.py).

Tests each check function independently with synthetic org text so they
run without the full corpus present.
"""
from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load_linter():
    script = ROOT / "context" / "tools" / "context_lint.py"
    if not script.exists():
        raise unittest.SkipTest("context_lint.py not present")
    spec = importlib.util.spec_from_file_location("context_lint", script)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class TestLineOf(unittest.TestCase):
    def test_first_line(self):
        lint = load_linter()
        self.assertEqual(lint.line_of("hello\nworld", 0), 1)

    def test_second_line(self):
        lint = load_linter()
        self.assertEqual(lint.line_of("hello\nworld", 6), 2)

    def test_offset_in_newline(self):
        lint = load_linter()
        self.assertEqual(lint.line_of("hello\nworld\nend", 12), 3)


class TestSplitRecordRefs(unittest.TestCase):
    def test_single_ref(self):
        lint = load_linter()
        result = lint.split_record_refs("src:foo.c from:obs.some-thing blocks:")
        self.assertIn("obs.some-thing", result)

    def test_multi_comma_refs(self):
        lint = load_linter()
        result = lint.split_record_refs("src:foo.c from:obs.a,obs.b supports:obs.c")
        self.assertIn("obs.a", result)
        self.assertIn("obs.b", result)
        self.assertIn("obs.c", result)

    def test_empty(self):
        lint = load_linter()
        self.assertEqual(lint.split_record_refs("src:foo.c"), [])


class TestIsInfoFile(unittest.TestCase):
    def test_info_prefix(self):
        lint = load_linter()
        self.assertTrue(lint.is_info_file("info/booleans.org"))

    def test_nested_info(self):
        lint = load_linter()
        self.assertTrue(lint.is_info_file("context/info/foo.org"))

    def test_non_info(self):
        lint = load_linter()
        self.assertFalse(lint.is_info_file("build.org"))
        self.assertFalse(lint.is_info_file("category/index.org"))


class TestFindHeadingRanges(unittest.TestCase):
    def test_basic_headings(self):
        lint = load_linter()
        text = "* File\nbody\n** Sub\nsubbody\n"
        ranges = lint.find_heading_ranges(text)
        self.assertEqual(len(ranges), 2)
        self.assertIn("File", ranges[0][2])
        self.assertIn("Sub", ranges[1][2])
        body1 = text[ranges[0][0]:ranges[0][1]]
        self.assertIn("body", body1)
        body2 = text[ranges[1][0]:ranges[1][1]]
        self.assertIn("subbody", body2)


class TestCheckEmptyHeadings(unittest.TestCase):
    def test_empty_heading_reported(self):
        lint = load_linter()
        text = "* File\n\n* Empty\n\n** Not empty\ncontent\n"
        issues = lint.check_empty_headings(text, "test.org")
        files = [f for f, l, h in issues]
        lines = [l for f, l, h in issues]
        self.assertIn("test.org", files)

    def test_info_exempt(self):
        lint = load_linter()
        text = "* File\n\n* Empty\n"
        issues = lint.check_empty_headings(text, "info/test.org")
        self.assertEqual(issues, [])


class TestCheckDescriptions(unittest.TestCase):
    def test_heading_with_id_missing_desc(self):
        lint = load_linter()
        text = """* Heading
:PROPERTIES:
:ID: monadc.context.foo
:END:
"""
        issues = lint.check_descriptions(text, "test.org")
        self.assertEqual(len(issues), 1)
        self.assertEqual(issues[0][0], "test.org")

    def test_heading_with_id_and_desc_ok(self):
        lint = load_linter()
        text = """* Heading
:PROPERTIES:
:ID: monadc.context.foo
:CONTEXT_DESCRIPTION: Some description
:END:
"""
        issues = lint.check_descriptions(text, "test.org")
        self.assertEqual(issues, [])

    def test_heading_without_id_not_checked(self):
        lint = load_linter()
        text = "* Heading\nbody\n"
        issues = lint.check_descriptions(text, "test.org")
        self.assertEqual(issues, [])


class TestCheckOrphaned(unittest.TestCase):
    def test_no_orphans_when_all_linked(self):
        lint = load_linter()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("a.org", "b.org", "c.org", "context.org", "index.org", "connections.org"):
                (root / name).write_text("")
            (root / "context.org").write_text("[[file:index.org]]\n[[file:connections.org]]")
            (root / "index.org").write_text("[[file:a.org]]\n[[file:b.org]]\n[[file:c.org]]")
            (root / "connections.org").write_text("[[file:a.org]]\n[[file:b.org]]")
            files = lint.org_files(root)
            texts = [
                (root / "context.org", "context.org", (root / "context.org").read_text()),
                (root / "index.org", "index.org", (root / "index.org").read_text()),
                (root / "connections.org", "connections.org", (root / "connections.org").read_text()),
            ]
            orphans = lint.check_orphaned(files, texts, root)
            self.assertEqual(orphans, [])

    def test_orphan_detected(self):
        lint = load_linter()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("a.org", "b.org", "index.org"):
                (root / name).write_text("")
            (root / "index.org").write_text("[[file:a.org]]")
            files = lint.org_files(root)
            texts = [(root / "index.org"), "index.org", "[[file:a.org]]"]
            texts = [(root / "index.org", "index.org", "[[file:a.org]]")]
            orphans = lint.check_orphaned(files, texts, root)
            self.assertIn("b.org", orphans)


class TestCheckSrcRefs(unittest.TestCase):
    def test_existing_file_ok(self):
        lint = load_linter()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "foo.c").write_text("int x;\n")
            text = "src:foo.c"
            issues = lint.check_src_refs(text, "test.org", root)
            self.assertEqual(issues, [])

    def test_missing_file_reported(self):
        lint = load_linter()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            text = "src:nonexistent.c:10"
            issues = lint.check_src_refs(text, "test.org", root)
            self.assertEqual(len(issues), 1)
            self.assertIn("FILE NOT FOUND", issues[0][3])

    def test_line_number_exceeds_file(self):
        lint = load_linter()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "foo.c").write_text("int x;\n")
            text = "src:foo.c:999"
            issues = lint.check_src_refs(text, "test.org", root)
            self.assertEqual(len(issues), 1)
            self.assertIn("EXCEEDS", issues[0][3])

    def test_non_source_path_skipped(self):
        lint = load_linter()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            text = "src:user-request"
            issues = lint.check_src_refs(text, "test.org", root)
            self.assertEqual(issues, [])


class TestCheckTestContextRefs(unittest.TestCase):
    def test_known_id_ok(self):
        lint = load_linter()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            test_dir = root / "tests"
            test_dir.mkdir()
            (test_dir / "test.mon").write_text(";; TEST-CONTEXT: monadc.context.foo\n")
            known = {"monadc.context.foo"}
            issues = lint.check_test_context_refs(root, known)
            self.assertEqual(issues, [])

    def test_unknown_id_reported(self):
        lint = load_linter()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            test_dir = root / "tests"
            test_dir.mkdir()
            (test_dir / "test.mon").write_text(";; TEST-CONTEXT: monadc.context.nonexistent\n")
            known = {"monadc.context.foo"}
            issues = lint.check_test_context_refs(root, known)
            self.assertEqual(len(issues), 1)
            self.assertIn("nonexistent", issues[0][2])


class TestFixLastModified(unittest.TestCase):
    def test_stale_date_replaced(self):
        lint = load_linter()
        text = "#+LAST_MODIFIED: 2020-01-01\n"
        result = lint.fix_last_modified(text, Path("/dummy"), "test.org", Path("/dummy"))
        self.assertIsNotNone(result)
        from datetime import date
        today = date.today().strftime("%Y-%m-%d")
        self.assertIn(today, result)

    def test_current_date_unchanged(self):
        lint = load_linter()
        from datetime import date
        today = date.today().strftime("%Y-%m-%d")
        text = f"#+LAST_MODIFIED: {today}\n"
        result = lint.fix_last_modified(text, Path("/dummy"), "test.org", Path("/dummy"))
        self.assertIsNone(result)


class TestMainJsonOutput(unittest.TestCase):
    def test_json_output_valid(self):
        lint = load_linter()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            org_file = root / "test.org"
            org_file.write_text("#+TITLE: Test\n#+AUTHOR: Tester\n#+DATE: 2026-06-18\n#+LAST_MODIFIED: 2026-06-18\n#+FILETAGS: :test:\n\ncontent\n")
            import contextlib, io
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = lint.main([str(tmp)])
            output = buf.getvalue()
            self.assertIn("context files: 1", output)

    def test_skip_info(self):
        lint = load_linter()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "test.org").write_text("#+TITLE: Test\n#+AUTHOR: T\n#+DATE: 2026-06-18\n#+LAST_MODIFIED: 2026-06-18\n#+FILETAGS: :test:\n\n")
            info_dir = root / "info"
            info_dir.mkdir()
            (info_dir / "info.org").write_text("#+TITLE: Info\n#+AUTHOR: T\n#+DATE: 2026-06-18\n#+LAST_MODIFIED: 2026-06-18\n#+FILETAGS: :info:\n\n")
            import contextlib, io
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = lint.main(["--skip-info", str(tmp)])


if __name__ == "__main__":
    unittest.main()
