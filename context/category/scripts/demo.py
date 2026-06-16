#!/usr/bin/env python3
"""Small demonstration of category-context interfaces."""
from __future__ import annotations
from pathlib import Path
import argparse, json, sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from context_model import parse_context, search
from info_metrics import metrics

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("root", nargs="?", default="context")
    args = ap.parse_args()
    root = Path(args.root)
    C = parse_context(root)
    print("== Category model ==")
    v = C.validate()
    print(f"Objects: {v['objects']}  Edges: {v['edges']}  Unresolved: {len(v['unresolved_edges'])}")
    print("\n== Search: 'typed define wisp' ==")
    for r in search(C, "typed define wisp", 5):
        print(f"{r['score']:5.2f} {r['kind']:8} {r['id']} ({r['path']}:{r['line']})")
    print("\n== Information metrics ==")
    m = metrics(root)
    for k, val in m["entropy_bits"].items():
        print(f"H({k}) = {val} bits")
    print("\n== Example path category law ==")
    # Pick any resolved edge and show id ∘ f = f = f ∘ id.
    for e in C.edges.values():
        if e.src in C.objects and e.dst in C.objects:
            f = C.edge_path(e.id)
            ok = C.identity(f.src).compose(f) == f and f.compose(C.identity(f.dst)) == f
            print(f"identity law on {e.kind}: {e.src} -> {e.dst}: {'OK' if ok else 'FAIL'}")
            break
    return 0
if __name__ == "__main__":
    raise SystemExit(main())

# round4-query-demo
# Additional direct import demo intentionally kept at EOF so older callers keep working.
def demo_query_language(root='context'):
    from pathlib import Path
    from context_model import parse_context
    from query_language import evaluate
    C = parse_context(Path(root))
    for expr in ['typed define :Record', '@category/scripts :Script', '#reader ~', 'typed define ƒkind']:
        rows, _ = evaluate(C, expr, 3)
        print(f"\n(Ctx) ◗ {expr}")
        for r in rows:
            print(f"  {r.kind:<12} {r.id}")
