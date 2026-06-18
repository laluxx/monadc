#!/usr/bin/env python3
"""Core parser/model for the MonadC context category.

The model is intentionally dependency-free.  It parses enough Org syntax to
extract files, stable heading IDs, bracket records, links, and record-reference
edges.  The resulting typed multigraph generates the free category used by the
category context layer.
"""
from __future__ import annotations

from dataclasses import dataclass, asdict
from pathlib import Path
import argparse
import collections
import json
import math
import re
from typing import Iterable, Iterator

RE_TITLE = re.compile(r"^#\+TITLE:\s*(.*)$", re.M)
RE_TAGS = re.compile(r"^#\+FILETAGS:\s*(.*)$", re.M)
RE_HEADING = re.compile(r"^(\*+)\s+(.*?)\s*$", re.M)
RE_ID = re.compile(r"^:ID:\s*(\S+)\s*$", re.M)
RE_RECORD = re.compile(r"^\[(DOC|OBS|INF|DEC|TODO|THINK|IDEA|FIX)\s+id:([^\s\]]+)([^\]]*)\]", re.M)
RE_FILE_LINK = re.compile(r"\[\[file:([^\]\[]+?)(?:::?\*[^\]]*)?\](?:\[[^\]]*\])?\]")
RE_ID_LINK = re.compile(r"\[\[id:([^\]\[]+)\](?:\[[^\]]*\])?\]")
RE_REF = re.compile(r"\b(from|supports|supersedes|blocks|verifies):([^\s\]]+)")
RE_WORD = re.compile(r"[A-Za-z0-9_.:+/\-]+")

RECORD_PREFIX_KIND = {
    "doc": "DOC", "obs": "OBS", "inf": "INF", "dec": "DEC", "todo": "TODO",
    "think": "THINK", "idea": "IDEA", "fix": "FIX",
}

@dataclass(frozen=True)
class Obj:
    id: str
    kind: str
    label: str = ""
    path: str = ""
    line: int = 0
    meta: dict | None = None

@dataclass(frozen=True)
class Edge:
    id: str
    src: str
    dst: str
    kind: str
    label: str = ""
    path: str = ""
    line: int = 0
    meta: dict | None = None

@dataclass(frozen=True)
class PathMorphism:
    src: str
    dst: str
    edges: tuple[str, ...]

    def compose(self, after: "PathMorphism") -> "PathMorphism":
        """Return after ∘ self, i.e. self followed by after."""
        if self.dst != after.src:
            raise ValueError(f"not composable: {self.dst!r} != {after.src!r}")
        return PathMorphism(self.src, after.dst, self.edges + after.edges)

    @staticmethod
    def identity(obj: str) -> "PathMorphism":
        return PathMorphism(obj, obj, ())

class ContextCategory:
    def __init__(self, root: Path):
        self.root = Path(root)
        self.objects: dict[str, Obj] = {}
        self.edges: dict[str, Edge] = {}
        self.duplicates: dict[str, list[str]] = collections.defaultdict(list)
        self.unresolved: list[Edge] = []
        self.texts: dict[str, str] = {}

    def add_object(self, obj: Obj) -> None:
        if obj.id in self.objects:
            self.duplicates[obj.id].append(f"{obj.path}:{obj.line}")
        else:
            self.objects[obj.id] = obj

    def add_edge(self, edge: Edge) -> None:
        key = edge.id
        if key in self.edges:
            # Deterministic disambiguation for multiedges at the same line/kind.
            i = 2
            while f"{key}#{i}" in self.edges:
                i += 1
            edge = Edge(f"{key}#{i}", edge.src, edge.dst, edge.kind, edge.label, edge.path, edge.line, edge.meta)
        self.edges[edge.id] = edge

    def kind_of(self, obj_id: str) -> str:
        obj = self.objects.get(obj_id)
        return obj.kind if obj else "Unknown"

    def identity(self, obj_id: str) -> PathMorphism:
        if obj_id not in self.objects:
            raise KeyError(obj_id)
        return PathMorphism.identity(obj_id)

    def edge_path(self, edge_id: str) -> PathMorphism:
        e = self.edges[edge_id]
        return PathMorphism(e.src, e.dst, (edge_id,))

    def outgoing(self, obj_id: str) -> list[Edge]:
        return [e for e in self.edges.values() if e.src == obj_id and e.dst in self.objects]

    def paths(self, src: str, dst: str, max_depth: int = 3) -> list[PathMorphism]:
        if src not in self.objects or dst not in self.objects:
            return []
        out: list[PathMorphism] = []
        frontier = [PathMorphism.identity(src)]
        for _ in range(max_depth):
            nxt: list[PathMorphism] = []
            for p in frontier:
                for e in self.outgoing(p.dst):
                    q = p.compose(PathMorphism(e.src, e.dst, (e.id,)))
                    if q.dst == dst:
                        out.append(q)
                    nxt.append(q)
            frontier = nxt
        return out

    def validate(self) -> dict:
        unresolved = []
        for e in self.edges.values():
            if e.src not in self.objects or e.dst not in self.objects:
                unresolved.append(asdict(e))
        kind_counts = collections.Counter(o.kind for o in self.objects.values())
        edge_counts = collections.Counter(e.kind for e in self.edges.values())
        return {
            "objects": len(self.objects),
            "edges": len(self.edges),
            "object_kinds": dict(sorted(kind_counts.items())),
            "edge_kinds": dict(sorted(edge_counts.items())),
            "duplicates": dict(self.duplicates),
            "unresolved_edges": unresolved,
        }

    def to_json(self) -> dict:
        return {
            "objects": [asdict(o) for o in self.objects.values()],
            "edges": [asdict(e) for e in self.edges.values()],
            "validation": self.validate(),
        }

def line_of(text: str, off: int) -> int:
    return text[:off].count("\n") + 1

def org_files(root: Path) -> list[Path]:
    return sorted(p for p in root.rglob("*.org") if ".git" not in p.parts and not p.name.startswith(".#"))

def context_files(root: Path) -> list[Path]:
    """All regular files that should be addressable as File/Script/Report objects."""
    skip_suffixes = {".pyc"}
    out = []
    for p in root.rglob("*"):
        if not p.is_file() or ".git" in p.parts or "__pycache__" in p.parts:
            continue
        if p.suffix in skip_suffixes:
            continue
        out.append(p)
    return sorted(out)

def normalize_file_target(current: Path, root: Path, raw: str) -> str | None:
    target_part = raw.split("::", 1)[0]
    if not target_part or target_part.startswith(("http:", "https:", "/")):
        return None
    # Org links in context/category/*.org are relative to that file. Preserve if inside root.
    target = (current.parent / target_part).resolve()
    try:
        rel = target.relative_to(root.resolve())
    except ValueError:
        return None
    return f"file:{rel.as_posix()}"

def split_refs(s: str) -> Iterator[tuple[str, str]]:
    for m in RE_REF.finditer(s):
        kind = m.group(1)
        for raw in m.group(2).split(','):
            ref = raw.strip().strip(',.;')
            if ref:
                yield kind, ref

def parse_context(root: Path) -> ContextCategory:
    root = Path(root)
    C = ContextCategory(root)
    # Every file under context is referentiable as an object, not only Org.
    for path in context_files(root):
        rel = path.relative_to(root).as_posix()
        file_id = f"file:{rel}"
        if rel.endswith(".py") or "/scripts/" in rel:
            kind = "Script"
        elif "generated" in rel:
            kind = "Report"
        else:
            kind = "File"
        label = rel
        meta = {"suffix": path.suffix}
        if rel.endswith(".org"):
            text0 = path.read_text(errors="replace")
            title0 = RE_TITLE.search(text0)
            tags0 = RE_TAGS.search(text0)
            label = title0.group(1).strip() if title0 else rel
            meta["tags"] = tags0.group(1).strip() if tags0 else ""
        C.add_object(Obj(file_id, kind, label, rel, 1, meta))

    for path in org_files(root):
        rel = path.relative_to(root).as_posix()
        text = path.read_text(errors="replace")
        C.texts[rel] = text
        file_id = f"file:{rel}"
        for m in RE_ID.finditer(text):
            hid = m.group(1).strip()
            ln = line_of(text, m.start())
            C.add_object(Obj(hid, "Heading", hid, rel, ln, {}))
            C.add_edge(Edge(f"contains:{file_id}->{hid}:{ln}", file_id, hid, "contains", "contains", rel, ln, {}))
        for m in RE_RECORD.finditer(text):
            typ, rid, rest = m.group(1), m.group(2).strip(), m.group(3)
            ln = line_of(text, m.start())
            C.add_object(Obj(rid, "Record", rid, rel, ln, {"record_type": typ}))
            C.add_edge(Edge(f"contains:{file_id}->{rid}:{ln}", file_id, rid, "contains", "contains", rel, ln, {}))
            for kind, target in split_refs(rest):
                C.add_edge(Edge(f"{kind}:{rid}->{target}:{ln}", rid, target, kind, kind, rel, ln, {}))
        for m in RE_FILE_LINK.finditer(text):
            target = normalize_file_target(path, root, m.group(1))
            if target:
                ln = line_of(text, m.start())
                C.add_edge(Edge(f"file-link:{file_id}->{target}:{ln}", file_id, target, "file-link", "file-link", rel, ln, {}))
        for m in RE_ID_LINK.finditer(text):
            target = m.group(1).strip()
            ln = line_of(text, m.start())
            C.add_edge(Edge(f"id-link:{file_id}->{target}:{ln}", file_id, target, "id-link", "id-link", rel, ln, {}))
    # Add taxonomy concepts from category taxonomy table if present.
    concept_names = [
        "root", "compiler-phase", "frontend", "reader", "wisp", "macro", "middle-end",
        "infer", "dep", "module", "backend", "codegen", "runtime", "meta",
        "test-system", "context-system", "taxonomy", "information", "type-system",
    ]
    for name in concept_names:
        C.add_object(Obj(f"concept:{name}", "Concept", name, "category/taxonomy.org", 0, {}))
    refine_edges = [
        ("frontend", "compiler-phase"), ("reader", "frontend"), ("wisp", "frontend"),
        ("macro", "frontend"), ("middle-end", "compiler-phase"), ("infer", "middle-end"),
        ("dep", "middle-end"), ("module", "middle-end"), ("backend", "compiler-phase"),
        ("codegen", "backend"), ("runtime", "backend"), ("test-system", "meta"),
        ("context-system", "meta"), ("taxonomy", "context-system"),
        ("information", "context-system"), ("type-system", "context-system"),
    ]
    for a, b in refine_edges:
        C.add_edge(Edge(f"refines:concept:{a}->concept:{b}", f"concept:{a}", f"concept:{b}", "refines", "refines", "category/taxonomy.org", 0, {}))
    return C

def shannon(counter: collections.Counter) -> float:
    total = sum(counter.values())
    if not total:
        return 0.0
    return -sum((n/total)*math.log2(n/total) for n in counter.values() if n)

def tokenize(text: str) -> list[str]:
    return [w.lower() for w in RE_WORD.findall(text)]

def search(C: ContextCategory, query: str, limit: int = 10, kind: str | None = None) -> list[dict]:
    terms = tokenize(query)
    if not terms:
        return []
    results = []
    for obj in C.objects.values():
        if kind and obj.kind.lower() != kind.lower():
            continue
        blob = " ".join([obj.id, obj.kind, obj.label, obj.path, json.dumps(obj.meta or {}, sort_keys=True)])
        if obj.path in C.texts and obj.kind in {"File", "Script"}:
            blob += "\n" + C.texts[obj.path][:20000]
        low = blob.lower()
        score = 0.0
        for t in terms:
            if t in obj.id.lower():
                score += 8.0
            if t in obj.label.lower():
                score += 5.0
            score += min(low.count(t), 8) * (1.0 + 1.0 / max(1, len(t)))
        if score:
            neigh = len(C.outgoing(obj.id))
            results.append({"id": obj.id, "kind": obj.kind, "path": obj.path, "line": obj.line, "score": round(score + min(neigh, 5)*0.1, 3), "label": obj.label})
    return sorted(results, key=lambda r: (-r["score"], r["id"]))[:limit]

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", default="context")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    C = parse_context(Path(args.root))
    data = C.to_json()
    if args.json:
        print(json.dumps(data, indent=2, sort_keys=True))
    else:
        v = data["validation"]
        print(f"objects: {v['objects']}")
        print(f"edges: {v['edges']}")
        print(f"unresolved_edges: {len(v['unresolved_edges'])}")
        print(f"duplicates: {len(v['duplicates'])}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
