#!/usr/bin/env python3
"""Search the context category with a small dependency-free ranker."""
from __future__ import annotations
from pathlib import Path
import argparse, json, sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from context_model import parse_context, search

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("root", help="context root")
    ap.add_argument("query", help="search query")
    ap.add_argument("--limit", type=int, default=10)
    ap.add_argument("--kind", help="object kind filter, e.g. File, Heading, Record")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    C = parse_context(Path(args.root))
    rows = search(C, args.query, args.limit, args.kind)
    if args.json:
        print(json.dumps({"query": args.query, "results": rows}, indent=2, sort_keys=True))
    else:
        print(f"query: {args.query}")
        for r in rows:
            loc = f"{r['path']}:{r['line']}" if r['line'] else r['path']
            print(f"{r['score']:6.2f}  {r['kind']:8}  {r['id']}  {loc}")
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
