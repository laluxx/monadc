#!/usr/bin/env python3
"""Lint the MonadC context garden.

Checks intentionally stay simple and text-based so the linter can run anywhere
without Emacs/org-mode dependencies. Default mode enforces only checks that
should pass inside a context-only tar. Optional stricter modes can validate
record references and local source links when the full repository is present.
"""
from __future__ import annotations

import argparse
import collections
import json
import re
import sys
from pathlib import Path

RE_ID = re.compile(r"^:ID:\s*(\S+)\s*$", re.M)
RE_RECORD = re.compile(r"^\[(DOC|OBS|INF|DEC|TODO|THINK|IDEA|FIX)\s+id:([^\s\]]+)([^\]]*)\]", re.M)
RE_FILE_LINK = re.compile(r"\[\[file:([^\]\[]+?)(?:::?\*[^\]]*)?\](?:\[[^\]]*\])?\]")
RE_ID_LINK = re.compile(r"\[\[id:([^\]\[]+)\](?:\[[^\]]*\])?\]")
RE_RECORD_REF = re.compile(r"\b(?:from|supersedes|blocks|supports|verifies):([^\s\]]+)")
RE_TITLE = re.compile(r"^#\+TITLE:\s*(.*)$", re.M)
RE_HEADING = re.compile(r"^\*+\s+", re.M)
RE_TAGS = re.compile(r"^#\+FILETAGS:\s*(.*)$", re.M)
REQUIRED_HEADERS = ["#+TITLE:", "#+AUTHOR:", "#+DATE:", "#+LAST_MODIFIED:", "#+FILETAGS:"]


def line_of(text: str, offset: int) -> int:
    return text[:offset].count("\n") + 1


def org_files(root: Path) -> list[Path]:
    return sorted(p for p in root.rglob("*.org") if ".git" not in p.parts)


def split_record_refs(raw: str) -> list[str]:
    out: list[str] = []
    for m in RE_RECORD_REF.finditer(raw):
        token = m.group(1).strip().strip(",.;")
        out.extend(x.strip().strip(",.;") for x in token.split(",") if x.strip())
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="lint MonadC context org files")
    ap.add_argument("root", nargs="?", default="context", help="context directory")
    ap.add_argument("--strict-links", action="store_true", help="fail on local broken file links including repository-relative links")
    ap.add_argument("--check-record-refs", action="store_true", help="fail when record from:/supersedes:/blocks:/supports:/verifies: refs do not resolve to record or heading IDs")
    ap.add_argument("--json", action="store_true", help="emit a JSON summary instead of text")
    args = ap.parse_args()

    root = Path(args.root)
    files = org_files(root)
    ids: dict[str, list[tuple[str, int]]] = collections.defaultdict(list)
    records: dict[str, list[tuple[str, str, int]]] = collections.defaultdict(list)
    header_gaps: list[tuple[str, list[str]]] = []
    broken_links: list[tuple[str, int, str]] = []
    broken_id_links: list[tuple[str, int, str]] = []
    broken_record_refs: list[tuple[str, int, str]] = []
    file_rows: list[dict[str, object]] = []
    record_ref_uses: list[tuple[str, int, str]] = []
    totals = {"lines": 0, "headings": 0, "records": 0, "ids": 0}

    texts: list[tuple[Path, str, str]] = []
    for path in files:
        text = path.read_text(errors="replace")
        rel = str(path.relative_to(root))
        texts.append((path, rel, text))
        totals["lines"] += text.count("\n") + 1
        headings = len(RE_HEADING.findall(text))
        totals["headings"] += headings
        title = RE_TITLE.search(text)
        tags = RE_TAGS.search(text)
        missing = [h for h in REQUIRED_HEADERS if h not in text]
        if missing:
            header_gaps.append((rel, missing))
        file_id_count = 0
        file_record_count = 0
        for m in RE_ID.finditer(text):
            ids[m.group(1)].append((rel, line_of(text, m.start())))
            totals["ids"] += 1
            file_id_count += 1
        for m in RE_RECORD.finditer(text):
            records[m.group(2)].append((m.group(1), rel, line_of(text, m.start())))
            totals["records"] += 1
            file_record_count += 1
            for ref in split_record_refs(m.group(3)):
                record_ref_uses.append((rel, line_of(text, m.start()), ref))
        file_rows.append({
            "file": rel,
            "title": title.group(1).strip() if title else "",
            "tags": tags.group(1).strip() if tags else "",
            "lines": text.count("\n") + 1,
            "headings": headings,
            "ids": file_id_count,
            "records": file_record_count,
        })

    known_ids = set(ids)
    known_records = set(records)

    for path, rel, text in texts:
        for m in RE_FILE_LINK.finditer(text):
            raw = m.group(1)
            target_part = raw.split("::", 1)[0]
            if not target_part or target_part.startswith(("http:", "https:", "/")):
                continue
            if target_part.startswith("..") and not args.strict_links:
                continue
            target = (path.parent / target_part).resolve()
            try:
                target.relative_to(root.resolve())
            except ValueError:
                if not args.strict_links:
                    continue
            if not target.exists():
                broken_links.append((rel, line_of(text, m.start()), raw))
        for m in RE_ID_LINK.finditer(text):
            target_id = m.group(1).strip()
            if target_id not in known_ids:
                broken_id_links.append((rel, line_of(text, m.start()), target_id))

    for rel, line, ref in record_ref_uses:
        # Record references are allowed to point to record IDs or heading IDs.
        # Ignore obvious non-ID placeholders/paths.
        if ":" in ref and not ref.startswith(("obs.", "inf.", "dec.", "doc.", "todo.", "think.", "idea.", "fix.", "monadc.context.")):
            continue
        if ref not in known_records and ref not in known_ids:
            broken_record_refs.append((rel, line, ref))

    dup_ids = {k: v for k, v in ids.items() if len(v) > 1}
    dup_records = {k: v for k, v in records.items() if len(v) > 1}

    summary = {
        "context_files": len(files),
        "lines": totals["lines"],
        "headings": totals["headings"],
        "heading_ids": totals["ids"],
        "records": totals["records"],
        "duplicate_heading_ids": dup_ids,
        "duplicate_record_ids": dup_records,
        "header_gaps": header_gaps,
        "broken_local_file_links": broken_links,
        "broken_id_links": broken_id_links,
        "broken_record_refs": broken_record_refs,
        "files": file_rows,
    }

    if args.json:
        def clean(obj):
            if isinstance(obj, tuple):
                return list(obj)
            if isinstance(obj, dict):
                return {k: clean(v) for k, v in obj.items()}
            if isinstance(obj, list):
                return [clean(v) for v in obj]
            return obj
        print(json.dumps(clean(summary), indent=2, sort_keys=True))
    else:
        print(f"context files: {len(files)}")
        print(f"lines: {totals['lines']}")
        print(f"headings: {totals['headings']}")
        print(f"heading IDs: {totals['ids']}")
        print(f"records: {totals['records']}")
        print(f"duplicate heading IDs: {len(dup_ids)}")
        print(f"duplicate record IDs: {len(dup_records)}")
        print(f"header gaps: {len(header_gaps)}")
        print(f"broken local file links: {len(broken_links)}")
        print(f"broken id links: {len(broken_id_links)}")
        if args.check_record_refs:
            print(f"broken record refs: {len(broken_record_refs)}")

        if dup_ids:
            print("\nDuplicate heading IDs:")
            for key, locs in sorted(dup_ids.items()):
                print(f"  {key}: {locs}")
        if dup_records:
            print("\nDuplicate record IDs:")
            for key, locs in sorted(dup_records.items()):
                print(f"  {key}: {locs}")
        if header_gaps:
            print("\nHeader gaps:")
            for rel, missing in header_gaps:
                print(f"  {rel}: {', '.join(missing)}")
        if broken_links:
            print("\nBroken local file links:")
            for rel, line, target in broken_links:
                print(f"  {rel}:{line}: {target}")
        if broken_id_links:
            print("\nBroken id links:")
            for rel, line, target in broken_id_links:
                print(f"  {rel}:{line}: {target}")
        if args.check_record_refs and broken_record_refs:
            print("\nBroken record references:")
            for rel, line, ref in broken_record_refs:
                print(f"  {rel}:{line}: {ref}")

    fail = bool(dup_ids or dup_records or header_gaps or (args.strict_links and broken_links) or broken_id_links or (args.check_record_refs and broken_record_refs))
    return 1 if fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
