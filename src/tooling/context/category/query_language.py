#!/usr/bin/env python3
"""Compact category query language for the MonadC context garden.

The language is designed for a TUI prompt, not for SQL-style reporting.  It is
small, mnemonic, and graph/category aware:

    typed define :Record        text search, then kind filter
    @wisp.org :Record           path filter
    ?doc.category.index.purpose exact/substr object focus
    wisp >                     outgoing codomains of wisp hits
    wisp >contains             outgoing codomains by edge kind
    typed define ~             one-hop neighborhood
    ?A ?B ∘                    paths from A to B, shown as morphism objects
    reader ƒkind               functor/projection: group by object kind

The parser intentionally remains dependency-free and deterministic.  It returns
ranked ObjectView rows or virtual PathView/GroupView rows that the TUI can draw
as rectangles.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import collections
import json
import math
import re
import shlex
import sys
from typing import Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from context_model import ContextCategory, Obj, Edge, PathMorphism, parse_context, search as text_search, tokenize

@dataclass(frozen=True)
class View:
    id: str
    kind: str
    label: str
    path: str = ""
    line: int = 0
    score: float = 0.0
    detail: str = ""
    obj_id: str | None = None
    meta: dict | None = None

class QueryError(Exception):
    pass

GLYPH_HELP = {
    "words": "plain words fuzzy-search id/label/path/metadata/text",
    ":Kind": "filter current rows by object kind, e.g. :Record :Heading :Script",
    "@path": "filter by path fragment, e.g. @wisp.org @category/scripts",
    "?id": "focus object by exact id or substring, e.g. ?concept:reader",
    "#concept": "focus/filter concept objects, e.g. #reader",
    ">": "map selected objects through outgoing morphisms to codomains",
    ">kind": "outgoing morphisms filtered by edge kind, e.g. >contains >id-link",
    "<": "map selected objects through incoming morphisms to domains",
    "<kind": "incoming morphisms filtered by edge kind",
    "~": "one-hop neighborhood: selected ∪ domains ∪ codomains",
    "∘": "compose two focused objects into bounded paths from first to second",
    "*N": "closure radius for the next graph op, e.g. *2 ~",
    "ƒkind": "functor/projection; currently kind/path/file/concept/edge",
    "!N": "limit result count, e.g. !30",
    "=text": "exact substring filter on id/label/path/detail",
    "help": "show this table",
}

DEFAULT_LIMIT = 30
RE_KIND = re.compile(r"^:[A-Za-z][A-Za-z0-9_-]*$")
RE_LIMIT = re.compile(r"^!([0-9]{1,4})$")
RE_RADIUS = re.compile(r"^\*([0-9]{1,2})$")


def tokenize_query(expr: str) -> list[str]:
    expr = expr.replace("◦", "∘")
    if not expr.strip():
        return []
    try:
        return shlex.split(expr)
    except ValueError:
        # During live typing quotes are often temporarily unbalanced; fall back
        # to whitespace splitting instead of making the TUI feel brittle.
        return expr.split()


def object_to_view(C: ContextCategory, obj: Obj, score: float = 0.0, detail: str = "") -> View:
    outgoing = len(C.outgoing(obj.id))
    incoming = sum(1 for e in C.edges.values() if e.dst == obj.id and e.src in C.objects)
    d = detail or f"{obj.kind} · out:{outgoing} in:{incoming}"
    return View(obj.id, obj.kind, obj.label or obj.id, obj.path, obj.line, score, d, obj.id, obj.meta or {})


def edge_to_detail(C: ContextCategory, e: Edge) -> str:
    return f"{e.kind}: {e.src} → {e.dst} @ {e.path}:{e.line}"


def path_to_view(C: ContextCategory, p: PathMorphism, idx: int = 0) -> View:
    if not p.edges:
        label = f"id({p.src})"
        detail = "identity morphism"
    else:
        label = f"{p.src} ⟶ {p.dst}"
        detail = " ∘ ".join(p.edges)
    return View(f"path:{p.src}->{p.dst}:{idx}", "MorphismPath", label, "", 0, max(1.0, 20.0-len(p.edges)), detail, None, {"src": p.src, "dst": p.dst, "edges": list(p.edges)})


def unique_views(views: Iterable[View]) -> list[View]:
    out: list[View] = []
    seen: set[str] = set()
    for v in views:
        key = v.id
        if key not in seen:
            seen.add(key)
            out.append(v)
    return out


def sort_views(rows: Iterable[View]) -> list[View]:
    return sorted(rows, key=lambda v: (-v.score, v.kind, v.id))


def all_objects(C: ContextCategory) -> list[View]:
    return [object_to_view(C, o, 0.1) for o in C.objects.values()]


def focus(C: ContextCategory, raw: str) -> list[View]:
    q = raw[1:] if raw.startswith("?") else raw
    if not q:
        return []
    if q in C.objects:
        return [object_to_view(C, C.objects[q], 100.0)]
    # Allow short, fast human fragments.
    low = q.lower()
    rows = []
    for obj in C.objects.values():
        blob = f"{obj.id} {obj.label} {obj.path}".lower()
        if low in blob:
            score = 50.0
            if low in obj.id.lower(): score += 20
            if low in obj.path.lower(): score += 5
            rows.append(object_to_view(C, obj, score))
    return sort_views(rows)[:DEFAULT_LIMIT]


def concept(C: ContextCategory, raw: str) -> list[View]:
    q = raw[1:].lower()
    candidates = [f"concept:{q}", q]
    for c in candidates:
        if c in C.objects:
            return [object_to_view(C, C.objects[c], 100.0)]
    rows = [object_to_view(C, o, 40.0) for o in C.objects.values() if o.kind == "Concept" and q in f"{o.id} {o.label}".lower()]
    return sort_views(rows)


def apply_text_search(C: ContextCategory, terms: list[str], limit: int) -> list[View]:
    query = " ".join(terms).strip()
    if not query:
        return []
    return [View(r["id"], r["kind"], r["label"], r["path"], int(r["line"]), float(r["score"]), f"score {r['score']}", r["id"], {}) for r in text_search(C, query, max(limit, DEFAULT_LIMIT))]


def filter_kind(rows: list[View], kind: str) -> list[View]:
    low = kind.lower()
    return [r for r in rows if r.kind.lower() == low]


def filter_path(rows: list[View], frag: str) -> list[View]:
    low = frag.lower()
    return [r for r in rows if low in (r.path or "").lower() or low in r.id.lower()]


def filter_exact(rows: list[View], frag: str) -> list[View]:
    low = frag.lower()
    return [r for r in rows if low in f"{r.id} {r.label} {r.path} {r.detail}".lower()]


def graph_step(C: ContextCategory, rows: list[View], direction: str, edge_kind: str | None = None, radius: int = 1, include_self: bool = False) -> list[View]:
    current = [r.obj_id for r in rows if r.obj_id in C.objects]
    seen: set[str] = set(current if include_self else [])
    frontier = set(current)
    for _ in range(max(1, radius)):
        nxt: set[str] = set()
        for oid in frontier:
            if direction in {">", "~"}:
                for e in C.outgoing(oid):
                    if edge_kind and e.kind != edge_kind:
                        continue
                    if e.dst in C.objects:
                        nxt.add(e.dst)
            if direction in {"<", "~"}:
                for e in C.edges.values():
                    if e.dst == oid and e.src in C.objects:
                        if edge_kind and e.kind != edge_kind:
                            continue
                        nxt.add(e.src)
        seen |= nxt
        frontier = nxt
        if not frontier:
            break
    return [object_to_view(C, C.objects[oid], 10.0, "reached by graph operation") for oid in seen if oid in C.objects]


def group_by(C: ContextCategory, rows: list[View], what: str) -> list[View]:
    what = what.lower().strip(":")
    groups: dict[str, list[View]] = collections.defaultdict(list)
    if what in {"kind", "k"}:
        for r in rows: groups[r.kind].append(r)
    elif what in {"path", "file", "p"}:
        for r in rows: groups[r.path or "<virtual>"].append(r)
    elif what in {"concept", "c"}:
        # Concepts are approximated by path/tags/id fragments. This is a
        # projection/functor that forgets individual node identity.
        for r in rows:
            label = "uncategorized"
            low = f"{r.id} {r.path} {r.label}".lower()
            for key in ["reader", "wisp", "infer", "codegen", "runtime", "tests", "category", "type", "module", "macro"]:
                if key in low:
                    label = key; break
            groups[label].append(r)
    elif what in {"edge", "e"}:
        for r in rows:
            if r.obj_id in C.objects:
                outs = C.outgoing(r.obj_id)
                if outs:
                    for e in outs: groups[e.kind].append(r)
                else:
                    groups["terminal"].append(r)
    else:
        for r in rows: groups["all"].append(r)
    out: list[View] = []
    for k, vs in sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        sample = ", ".join(v.id for v in vs[:4])
        out.append(View(f"group:{what}:{k}", "Projection", f"{what}:{k}", "", 0, float(len(vs)), f"{len(vs)} objects · {sample}", None, {"count": len(vs)}))
    return out


def compose_query(C: ContextCategory, rows: list[View], limit: int) -> list[View]:
    objs = [r.obj_id for r in rows if r.obj_id in C.objects]
    if len(objs) < 2:
        return []
    src, dst = objs[0], objs[1]
    paths = C.paths(src, dst, max_depth=4)
    if not paths:
        return [View(f"path:none:{src}->{dst}", "MorphismPath", f"{src} ⇸ {dst}", "", 0, 0.0, "no path found up to depth 4", None, {"src": src, "dst": dst})]
    return [path_to_view(C, p, i) for i, p in enumerate(paths[:limit])]


def evaluate(C: ContextCategory, expr: str, limit: int = DEFAULT_LIMIT) -> tuple[list[View], dict]:
    tokens = tokenize_query(expr)
    if not tokens:
        return [], {"tokens": [], "help": GLYPH_HELP}
    if tokens == ["help"] or tokens == ["?"]:
        rows = [View(f"help:{k}", "Help", k, "", 0, 1.0, v) for k, v in GLYPH_HELP.items()]
        return rows, {"tokens": tokens, "mode": "help"}

    rows: list[View] = []
    pending_terms: list[str] = []
    next_radius = 1
    explicit_limit = limit

    def flush_terms():
        nonlocal rows, pending_terms
        if pending_terms:
            found = apply_text_search(C, pending_terms, explicit_limit)
            if rows:
                allowed = {r.id for r in found}
                rows = [r for r in rows if r.id in allowed]
            else:
                rows = found
            pending_terms = []

    i = 0
    while i < len(tokens):
        tok = tokens[i]
        mlim = RE_LIMIT.match(tok)
        mrad = RE_RADIUS.match(tok)
        if mlim:
            explicit_limit = max(1, min(500, int(mlim.group(1))))
        elif mrad:
            next_radius = max(1, min(6, int(mrad.group(1))))
        elif RE_KIND.match(tok):
            flush_terms()
            if not rows: rows = all_objects(C)
            rows = filter_kind(rows, tok[1:])
        elif tok.startswith("@") and len(tok) > 1:
            flush_terms()
            if not rows: rows = all_objects(C)
            rows = filter_path(rows, tok[1:])
        elif tok.startswith("=") and len(tok) > 1:
            flush_terms()
            if not rows: rows = all_objects(C)
            rows = filter_exact(rows, tok[1:])
        elif tok.startswith("?") and len(tok) > 1:
            flush_terms()
            # Preserve focus order so `?A ?B ∘` means paths A → B.
            rows.extend(focus(C, tok))
            rows = unique_views(rows)
        elif tok.startswith("#") and len(tok) > 1:
            flush_terms()
            rows.extend(concept(C, tok))
            rows = unique_views(rows)
        elif tok in {">", "<", "~"} or (tok.startswith(">") and len(tok) > 1) or (tok.startswith("<") and len(tok) > 1):
            flush_terms()
            if not rows: rows = all_objects(C)
            direction = tok[0]
            edge_kind = tok[1:] if len(tok) > 1 else None
            rows = graph_step(C, rows, direction, edge_kind=edge_kind or None, radius=next_radius, include_self=(direction == "~"))
            next_radius = 1
        elif tok in {"∘", "o", "compose"}:
            flush_terms()
            rows = compose_query(C, rows, explicit_limit)
        elif tok.startswith("ƒ") or tok.startswith("f:"):
            flush_terms()
            if not rows: rows = all_objects(C)
            what = tok[1:] if tok.startswith("ƒ") else tok[2:]
            rows = group_by(C, rows, what or "kind")
        else:
            pending_terms.append(tok)
        i += 1
    flush_terms()
    rows = unique_views(sort_views(rows))[:explicit_limit]
    meta = {"tokens": tokens, "limit": explicit_limit, "count": len(rows), "help": GLYPH_HELP}
    return rows, meta


def explain(expr: str) -> str:
    toks = tokenize_query(expr)
    bits = []
    for t in toks:
        if RE_KIND.match(t): bits.append(f"filter kind={t[1:]}")
        elif t.startswith('@'): bits.append(f"filter path contains {t[1:]!r}")
        elif t.startswith('?'): bits.append(f"focus {t[1:]!r}")
        elif t.startswith('#'): bits.append(f"concept {t[1:]!r}")
        elif t.startswith('>'): bits.append(f"outgoing {t[1:] or 'any'}")
        elif t.startswith('<'): bits.append(f"incoming {t[1:] or 'any'}")
        elif t == '~': bits.append("neighborhood")
        elif t in {'∘','o','compose'}: bits.append("compose paths")
        elif t.startswith('ƒ') or t.startswith('f:'): bits.append(f"project {t[1:] if t.startswith('ƒ') else t[2:]}")
        elif t.startswith('!'): bits.append(f"limit {t[1:]}")
        elif t.startswith('*'): bits.append(f"radius {t[1:]}")
        else: bits.append(f"search {t!r}")
    return " → ".join(bits)


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Run the MonadC context category query language")
    ap.add_argument("root", nargs="?", default="context")
    ap.add_argument("query", nargs="*", help="query expression, e.g. 'typed define :Record >'")
    ap.add_argument("--limit", type=int, default=DEFAULT_LIMIT)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--explain", action="store_true")
    args = ap.parse_args()
    expr = " ".join(args.query)
    C = parse_context(Path(args.root))
    rows, meta = evaluate(C, expr, args.limit)
    if args.explain:
        meta["explain"] = explain(expr)
    if args.json:
        print(json.dumps({"query": expr, "meta": meta, "results": [r.__dict__ for r in rows]}, indent=2, sort_keys=True))
    else:
        if args.explain:
            print(explain(expr))
        for r in rows:
            loc = f" {r.path}:{r.line}" if r.path else ""
            print(f"{r.score:6.2f} {r.kind:<13} {r.id}{loc}")
            if r.label and r.label != r.id:
                print(f"       {r.label}")
            if r.detail:
                print(f"       {r.detail}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
