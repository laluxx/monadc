#!/usr/bin/env python3
"""Render a deterministic Org report from the category model."""
from __future__ import annotations
from pathlib import Path
import argparse, datetime, json, sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from context_model import parse_context
from info_metrics import metrics

def render(root: Path) -> str:
    C = parse_context(root)
    m = metrics(root)
    lines = []
    lines.append("#+TITLE: Generated Category Report")
    lines.append("#+AUTHOR: context/category/scripts/render_docs.py")
    lines.append("#+DATE: 2026-06-16")
    lines.append("#+CREATED: 2026-06-16")
    lines.append("#+LAST_MODIFIED: 2026-06-16")
    lines.append("#+FILETAGS: :monadc:compiler:context:category:generated:report:")
    lines.append("#+PROPERTY: CONTEXT_VERSION 5")
    lines.append("")
    lines.append("* Summary")
    lines.append(":PROPERTIES:")
    lines.append(":ID: monadc.context.category.generated.summary")
    lines.append(":CONTEXT_KIND: generated-report")
    lines.append(":CONTEXT_STATUS: generated")
    lines.append(":SOURCE: context/category/scripts/render_docs.py")
    lines.append(":CONFIDENCE: high")
    lines.append(":END:")
    lines.append("")
    lines.append("[DOC id:doc.category.generated.summary src:context/category/scripts/render_docs.py conf:high]")
    lines.append("  Generated snapshot of the category graph and information metrics.")
    lines.append("")
    lines.append(f"- Objects: {m['objects']}")
    lines.append(f"- Edges: {m['edges']}")
    lines.append(f"- Unresolved edges: {len(m['validation']['unresolved_edges'])}")
    lines.append("")
    lines.append("* Entropy")
    lines.append(":PROPERTIES:")
    lines.append(":ID: monadc.context.category.generated.entropy")
    lines.append(":CONTEXT_KIND: generated-report")
    lines.append(":CONTEXT_STATUS: generated")
    lines.append(":SOURCE: context/category/scripts/info_metrics.py")
    lines.append(":CONFIDENCE: high")
    lines.append(":END:")
    lines.append("")
    lines.append("| Signal | Entropy bits |")
    lines.append("|--------+--------------|")
    for k, v in m["entropy_bits"].items():
        lines.append(f"| {k} | {v} |")
    lines.append("")
    lines.append("* Top Record Types")
    lines.append(":PROPERTIES:")
    lines.append(":ID: monadc.context.category.generated.record-types")
    lines.append(":CONTEXT_KIND: generated-report")
    lines.append(":CONTEXT_STATUS: generated")
    lines.append(":SOURCE: context/category/scripts/info_metrics.py")
    lines.append(":CONFIDENCE: high")
    lines.append(":END:")
    lines.append("")
    lines.append("| Type | Count |")
    lines.append("|------+-------|")
    for row in m["top"]["record_types"]:
        lines.append(f"| {row['key']} | {row['count']} |")
    lines.append("")
    return "\n".join(lines) + "\n"

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("root", nargs="?", default="context")
    ap.add_argument("--out", default="-")
    args = ap.parse_args()
    text = render(Path(args.root))
    if args.out == "-":
        print(text, end="")
    else:
        Path(args.out).write_text(text, encoding="utf-8")
        print(args.out)
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
