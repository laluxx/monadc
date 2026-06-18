#!/usr/bin/env python3
"""Tests for the context garden graph exporter (context/tools/context_graph.py)."""
from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load_graph():
    script = ROOT / "context" / "tools" / "context_graph.py"
    if not script.exists():
        raise unittest.SkipTest("context_graph.py not present")
    spec = importlib.util.spec_from_file_location("context_graph", script)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class TestGraphOutput(unittest.TestCase):
    def test_graph_output_is_valid_json(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "context" / "tools" / "context_graph.py"), "--skip-info", "--compact"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0)
        data = json.loads(result.stdout)
        self.assertIn("nodes", data)
        self.assertIn("edges", data)
        self.assertIn("counts", data)
        self.assertGreater(len(data["nodes"]), 0)

    def test_graph_stats_only(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "context" / "tools" / "context_graph.py"), "--stats-only"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("Nodes:", result.stdout)

    def test_graph_minimal_omits_raw_content(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "context" / "tools" / "context_graph.py"), "--minimal", "--compact"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0)
        data = json.loads(result.stdout)
        for node in data["nodes"]:
            self.assertNotIn("raw_content", node)

    def test_graph_skip_info_fewer_nodes(self):
        full = json.loads(subprocess.run(
            [sys.executable, str(ROOT / "context" / "tools" / "context_graph.py"), "--compact"],
            capture_output=True, text=True,
        ).stdout)
        skipped = json.loads(subprocess.run(
            [sys.executable, str(ROOT / "context" / "tools" / "context_graph.py"), "--skip-info", "--compact"],
            capture_output=True, text=True,
        ).stdout)
        # info/ adds many files, so skipped should have fewer
        self.assertLess(skipped["counts"]["nodes"], full["counts"]["nodes"])

    def test_help_prints_usage(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "context" / "tools" / "context_graph.py"), "--help"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("usage:", result.stdout)


if __name__ == "__main__":
    unittest.main()
