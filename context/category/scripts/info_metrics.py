#!/usr/bin/env python3
"""Compute information-theoretic metrics over the context category."""
from __future__ import annotations
from pathlib import Path
import argparse, collections, json, math, sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from context_model import parse_context, shannon

def top(counter, n=10):
    return [{"key": k, "count": v} for k, v in counter.most_common(n)]

def metrics(root: Path) -> dict:
    C = parse_context(root)
    obj_kinds = collections.Counter(o.kind for o in C.objects.values())
    edge_kinds = collections.Counter(e.kind for e in C.edges.values())
    record_types = collections.Counter((o.meta or {}).get("record_type", "") for o in C.objects.values() if o.kind == "Record")
    file_records = collections.Counter(o.path for o in C.objects.values() if o.kind == "Record")
    file_lines = {p: txt.count("\n")+1 for p, txt in C.texts.items()}
    record_density = []
    for f, n in file_records.items():
        record_density.append({"file": f, "records": n, "lines": file_lines.get(f, 1), "records_per_100_lines": round(100*n/max(file_lines.get(f, 1),1), 3)})
    record_density.sort(key=lambda r: (-r["records_per_100_lines"], r["file"]))
    return {
        "objects": len(C.objects),
        "edges": len(C.edges),
        "entropy_bits": {
            "object_kinds": round(shannon(obj_kinds), 4),
            "edge_kinds": round(shannon(edge_kinds), 4),
            "record_types": round(shannon(record_types), 4),
            "records_by_file": round(shannon(file_records), 4),
        },
        "top": {
            "object_kinds": top(obj_kinds),
            "edge_kinds": top(edge_kinds),
            "record_types": top(record_types),
            "record_files": top(file_records),
            "record_density": record_density[:10],
        },
        "validation": C.validate(),
    }

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("root", nargs="?", default="context")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    data = metrics(Path(args.root))
    if args.json:
        print(json.dumps(data, indent=2, sort_keys=True))
    else:
        print(f"objects: {data['objects']}")
        print(f"edges: {data['edges']}")
        print("entropy_bits:")
        for k, v in data["entropy_bits"].items():
            print(f"  {k}: {v}")
        print("top record types:")
        for row in data["top"]["record_types"]:
            print(f"  {row['key']}: {row['count']}")
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
