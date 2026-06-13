import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def load_visualizer():
    spec = importlib.util.spec_from_file_location("context_visualizer", ROOT / "context-visualizer.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ContextVisualizerTests(unittest.TestCase):
    def test_core_methods_include_usage_relations(self):
        visualizer = load_visualizer()

        methods = {method.name: method for method in visualizer.parse_core_methods()}

        self.assertIn("apply", methods)
        self.assertIn("foldl", methods["apply"].uses)
        self.assertIn("apply", methods["foldl"].used_by)
        self.assertTrue(methods["apply"].id.startswith("method:core:"))

    def test_compact_signature_removes_parameter_names(self):
        visualizer = load_visualizer()

        self.assertEqual(visualizer.compact_signature("[x :: Int] -> Int"), "Int -> Int")
        self.assertEqual(visualizer.compact_signature("[f : Fn] [x : Int] -> Int"), "Fn Int -> Int")

    def test_context_graph_indexes_docs_headings_and_new_corpora(self):
        visualizer = load_visualizer()

        graph = visualizer.parse_context_graph()
        nodes = {node["id"]: node for node in graph["nodes"]}

        self.assertIn("monadc.context.reader.char-escapes", nodes)
        self.assertEqual(nodes["monadc.context.reader.char-escapes"]["heading"], "Char Escape Parsing")
        self.assertIn("Char literals", nodes["monadc.context.reader.char-escapes"]["content"])
        self.assertIn("tests.codegen.rt-cffi-math-sqrt", nodes)
        self.assertIn("tests.refinement.refine_int_positive", nodes)
        self.assertIn("tests.interaction.interaction_module_cffi_refinement", nodes)

    def test_context_graph_exposes_documentation_search_contract(self):
        visualizer = load_visualizer()

        graph = visualizer.parse_context_graph()
        nodes = {node["id"]: node for node in graph["nodes"]}
        doc = nodes["doc.references.propositions-as-types"]

        self.assertEqual(doc["kind"], "documentation")
        self.assertEqual(doc["record_type"], "DOC")
        self.assertIn("Curry-Howard", doc["content"])
        self.assertIn("etc/pdfs/propositions-as-types.pdf", doc["source"])
        self.assertGreater(graph["stats"]["documentation"], 0)

    def test_context_graph_preserves_raw_org_for_rich_rendering(self):
        visualizer = load_visualizer()

        graph = visualizer.parse_context_graph()
        nodes = {node["id"]: node for node in graph["nodes"]}
        build_file = nodes["file:context/build.org"]
        build_heading = nodes["monadc.context.build.file-metadata"]

        self.assertIn("#+TITLE: Build and Installation Context", build_file["raw_content"])
        self.assertIn(":PROPERTIES:", build_heading["raw_content"])
        self.assertIn("[OBS id:obs.context.build.file", build_heading["raw_content"])

    def test_done_todo_rewrite_adds_org_closed_timestamp(self):
        visualizer = load_visualizer()
        lines = ["* TODO Polish visualizer", "body"]

        rewritten = visualizer.rewrite_todo_heading(lines, 0, "done", "2026-06-11 Thu 14:35")

        self.assertEqual(rewritten[0], "* DONE Polish visualizer")
        self.assertEqual(rewritten[1], "CLOSED: [2026-06-11 Thu 14:35]")
        self.assertEqual(rewritten[2], "body")

    def test_editor_command_uses_editor_env_and_line(self):
        visualizer = load_visualizer()

        command = visualizer.build_editor_command(ROOT / "context" / "visualizer.org", 12, "emacsclient")

        self.assertEqual(command[:3], ["emacsclient", "-n", "+12"])
        self.assertEqual(command[3], str(ROOT / "context" / "visualizer.org"))

    def test_resolve_repo_file_rejects_escape(self):
        visualizer = load_visualizer()

        with self.assertRaises(ValueError):
            visualizer.resolve_repo_file("../outside")


if __name__ == "__main__":
    unittest.main()
