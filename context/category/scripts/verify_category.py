#!/usr/bin/env python3
"""Verify that the context graph generates a valid free category."""
from __future__ import annotations

from pathlib import Path
import argparse
import json
import sys

# Allow running as a script without package installation.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from context_model import ContextCategory, PathMorphism, parse_context

SCHEMA_TYPES = {
    "contains": ({"File", "Script", "Report"}, {"Heading", "Record", "Script", "Report"}),
    "file-link": ({"File", "Script", "Heading", "Record"}, {"File", "Script"}),
    "id-link": ({"File", "Script", "Heading", "Record"}, {"Heading"}),
    "supports": ({"Record"}, {"Record", "Heading"}),
    "supersedes": ({"Record"}, {"Record", "Heading"}),
    "verifies": ({"Test", "Record"}, {"Heading", "Record"}),
    "blocks": ({"Record"}, {"Heading", "Record"}),
    "refines": ({"Concept"}, {"Concept"}),
    "classifies-as": ({"File", "Heading", "Record", "Test", "Script", "Report"}, {"Concept"}),
    "forgets-to": ({"File", "Heading", "Record", "Script", "Report", "Concept"}, {"Concept"}),
}

def check_identities(C: ContextCategory, sample_limit: int = 200) -> list[str]:
    errors = []
    for obj_id in list(C.objects)[:sample_limit]:
        ident = C.identity(obj_id)
        if ident.src != obj_id or ident.dst != obj_id or ident.edges != ():
            errors.append(f"bad identity for {obj_id}")
        for e in C.outgoing(obj_id)[:3]:
            f = C.edge_path(e.id)
            left = ident.compose(f)
            right = f.compose(C.identity(f.dst))
            if left != f:
                errors.append(f"left identity failed for {e.id}")
            if right != f:
                errors.append(f"right identity failed for {e.id}")
    return errors

def check_associativity() -> list[str]:
    # A tiny concrete category fragment proves the path implementation law.
    f = PathMorphism("A", "B", ("f",))
    g = PathMorphism("B", "C", ("g",))
    h = PathMorphism("C", "D", ("h",))
    left = f.compose(g).compose(h)
    right = f.compose(g.compose(h))
    return [] if left == right == PathMorphism("A", "D", ("f", "g", "h")) else ["associativity failed"]

def check_typed_edges(C: ContextCategory) -> list[str]:
    errors = []
    for e in C.edges.values():
        if e.src not in C.objects or e.dst not in C.objects:
            continue
        if e.kind not in SCHEMA_TYPES:
            continue
        domains, codomains = SCHEMA_TYPES[e.kind]
        sk, dk = C.kind_of(e.src), C.kind_of(e.dst)
        if sk not in domains or dk not in codomains:
            errors.append(f"{e.path}:{e.line}: {e.kind} has type {sk}->{dk}, expected {sorted(domains)}->{sorted(codomains)} ({e.src} -> {e.dst})")
    return errors

def run_self_test() -> list[str]:
    errors = []
    C = ContextCategory(Path("."))
    C.add_object(__import__('context_model').Obj("A", "Concept"))
    C.add_object(__import__('context_model').Obj("B", "Concept"))
    C.add_object(__import__('context_model').Obj("C", "Concept"))
    C.add_edge(__import__('context_model').Edge("f", "A", "B", "refines"))
    C.add_edge(__import__('context_model').Edge("g", "B", "C", "refines"))
    p = C.edge_path("f").compose(C.edge_path("g"))
    if p.src != "A" or p.dst != "C" or p.edges != ("f", "g"):
        errors.append("path composition self-test failed")
    errors.extend(check_identities(C))
    errors.extend(check_associativity())
    errors.extend(check_typed_edges(C))
    return errors

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("root", nargs="?", default="context")
    ap.add_argument("--self-test", action="store_true", help="also run internal path/category law tests")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--allow-unresolved", action="store_true", help="do not fail unresolved edges")
    args = ap.parse_args()

    C = parse_context(Path(args.root))
    validation = C.validate()
    errors = []
    if validation["duplicates"]:
        errors.append(f"duplicate object IDs: {validation['duplicates']}")
    if validation["unresolved_edges"] and not args.allow_unresolved:
        for e in validation["unresolved_edges"][:50]:
            errors.append(f"unresolved edge {e['kind']} {e['src']} -> {e['dst']} at {e['path']}:{e['line']}")
        if len(validation["unresolved_edges"]) > 50:
            errors.append(f"... {len(validation['unresolved_edges'])-50} more unresolved edges")
    errors.extend(check_identities(C))
    errors.extend(check_associativity())
    errors.extend(check_typed_edges(C))
    if args.self_test:
        errors.extend(run_self_test())

    result = {
        "ok": not errors,
        "objects": validation["objects"],
        "edges": validation["edges"],
        "object_kinds": validation["object_kinds"],
        "edge_kinds": validation["edge_kinds"],
        "unresolved_edges": len(validation["unresolved_edges"]),
        "errors": errors,
        "laws_checked": ["identity-left", "identity-right", "associativity-by-path-concat", "typed-generating-edges"],
    }
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(f"objects: {result['objects']}")
        print(f"edges: {result['edges']}")
        print(f"unresolved edges: {result['unresolved_edges']}")
        print("laws checked: " + ", ".join(result["laws_checked"]))
        print("status: " + ("OK" if result["ok"] else "FAIL"))
        if errors:
            print("\nErrors:")
            for e in errors:
                print("  - " + e)
    return 0 if not errors else 1

if __name__ == "__main__":
    raise SystemExit(main())
