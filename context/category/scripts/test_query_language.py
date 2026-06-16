#!/usr/bin/env python3
"""Tests for the category query language and terminal card renderer."""
from __future__ import annotations
from pathlib import Path
import tempfile
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from context_model import parse_context
from query_language import evaluate, explain

def box(title: str, body: str, width: int = 20) -> str:
    return '╭' + '─'*(width-2) + '╮\n' + title + '\n' + body + '\n' + '╰' + '─'*(width-2) + '╯'

MINI = """#+TITLE: Mini
#+AUTHOR: test
#+DATE: 2026-06-16
#+LAST_MODIFIED: 2026-06-16
#+FILETAGS: :mini:reader:wisp:
* Reader
:PROPERTIES:
:ID: monadc.context.reader.node
:END:
[DOC id:doc.reader.node src:mini.org conf:high]
  reader parses tokens
* Wisp
:PROPERTIES:
:ID: monadc.context.wisp.node
:END:
[OBS id:obs.wisp.typed-define supports:doc.reader.node src:mini.org conf:high]
  typed define sugar
"""

class QueryLanguageTests(unittest.TestCase):
    def with_ctx(self):
        d = tempfile.TemporaryDirectory()
        root = Path(d.name)
        (root/'mini.org').write_text(MINI)
        return d, parse_context(root)

    def test_text_and_kind_filter(self):
        d, C = self.with_ctx()
        self.addCleanup(d.cleanup)
        rows, meta = evaluate(C, 'typed define :Record', 10)
        self.assertTrue(any(r.id == 'obs.wisp.typed-define' for r in rows))

    def test_focus_and_outgoing(self):
        d, C = self.with_ctx(); self.addCleanup(d.cleanup)
        rows, _ = evaluate(C, '?obs.wisp.typed-define >supports', 10)
        self.assertTrue(any(r.id == 'doc.reader.node' for r in rows))

    def test_neighborhood(self):
        d, C = self.with_ctx(); self.addCleanup(d.cleanup)
        rows, _ = evaluate(C, '?doc.reader.node ~', 10)
        ids = {r.id for r in rows}
        self.assertIn('doc.reader.node', ids)
        self.assertIn('obs.wisp.typed-define', ids)

    def test_projection(self):
        d, C = self.with_ctx(); self.addCleanup(d.cleanup)
        rows, _ = evaluate(C, 'typed ƒkind', 10)
        self.assertTrue(any(r.kind == 'Projection' for r in rows))


    def test_focus_order_composition(self):
        d, C = self.with_ctx(); self.addCleanup(d.cleanup)
        # Add built-in taxonomy concepts are available from parse_context.
        rows, _ = evaluate(C, '?concept:reader ?concept:frontend ∘', 10)
        self.assertTrue(any(r.kind == 'MorphismPath' and 'concept:reader' in r.id and 'concept:frontend' in r.id for r in rows))

    def test_explain(self):
        self.assertIn('outgoing', explain('wisp >contains'))

    def test_box_renderer(self):
        s = box('Title', 'body text', width=20)
        self.assertIn('Title', s)
        self.assertIn('body', s)

if __name__ == '__main__':
    unittest.main()
