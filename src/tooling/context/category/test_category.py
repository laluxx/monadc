#!/usr/bin/env python3
"""Unit tests for the category context scripts."""
from __future__ import annotations
from pathlib import Path
import tempfile
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from context_model import Edge, Obj, ContextCategory, PathMorphism, parse_context, search
from verify_category import check_associativity, check_identities, check_typed_edges

class PathCategoryTests(unittest.TestCase):
    def test_identity_and_composition(self):
        f = PathMorphism('A','B',('f',))
        g = PathMorphism('B','C',('g',))
        self.assertEqual(PathMorphism.identity('A').compose(f), f)
        self.assertEqual(f.compose(PathMorphism.identity('B')), f)
        self.assertEqual(f.compose(g), PathMorphism('A','C',('f','g')))

    def test_associativity(self):
        self.assertEqual(check_associativity(), [])

class ParserTests(unittest.TestCase):
    def test_parse_minimal_org(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root/'a.org').write_text('''#+TITLE: A\n#+AUTHOR: t\n#+DATE: 2026-06-16\n#+LAST_MODIFIED: 2026-06-16\n#+FILETAGS: :x:\n* Node\n:PROPERTIES:\n:ID: monadc.context.test.node\n:END:\n[DOC id:doc.test.node src:a.org conf:high]\n''')
            C = parse_context(root)
            self.assertIn('file:a.org', C.objects)
            self.assertIn('monadc.context.test.node', C.objects)
            self.assertIn('doc.test.node', C.objects)
            self.assertEqual(C.validate()['unresolved_edges'], [])

    def test_search_finds_record(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root/'wisp.org').write_text('''#+TITLE: Wisp\n#+AUTHOR: t\n#+DATE: 2026-06-16\n#+LAST_MODIFIED: 2026-06-16\n#+FILETAGS: :wisp:\n* Typed define\n:PROPERTIES:\n:ID: monadc.context.wisp.typed-define\n:END:\n[OBS id:obs.wisp.typed-define src:wisp.org conf:high]\n  typed define sugar\n''')
            C = parse_context(root)
            rows = search(C, 'typed define', 3)
            self.assertTrue(any('typed-define' in r['id'] for r in rows))

class TypedEdgeTests(unittest.TestCase):
    def test_refines_concept_edge_is_typed(self):
        C = ContextCategory(Path('.'))
        C.add_object(Obj('concept:a','Concept'))
        C.add_object(Obj('concept:b','Concept'))
        C.add_edge(Edge('e','concept:a','concept:b','refines'))
        self.assertEqual(check_typed_edges(C), [])

    def test_bad_refines_edge_is_rejected(self):
        C = ContextCategory(Path('.'))
        C.add_object(Obj('file:a.org','File'))
        C.add_object(Obj('concept:b','Concept'))
        C.add_edge(Edge('e','file:a.org','concept:b','refines'))
        self.assertTrue(check_typed_edges(C))

if __name__ == '__main__':
    unittest.main()
