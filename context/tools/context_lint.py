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
from datetime import date
from pathlib import Path

RE_ID = re.compile(r"^:ID:\s*(\S+)\s*$", re.M)
RE_RECORD = re.compile(r"^\[(DOC|OBS|INF|DEC|TODO|THINK|IDEA|FIX)\s+id:([^\s\]]+)([^\]]*)\]", re.M)
RE_FILE_LINK = re.compile(r"\[\[file:([^\]\[]+?)(?:::?\*[^\]]*)?\](?:\[[^\]]*\])?\]")
RE_ID_LINK = re.compile(r"\[\[id:([^\]\[]+)\](?:\[[^\]]*\])?\]")
RE_RECORD_REF = re.compile(r"\b(?:from|supersedes|blocks|supports|verifies):([^\s\]]+)")
RE_TITLE = re.compile(r"^#\+TITLE:\s*(.*)$", re.M)
RE_HEADING = re.compile(r"^\*+\s+", re.M)
RE_TAGS = re.compile(r"^#\+FILETAGS:\s*(.*)$", re.M)
RE_LAST_MODIFIED = re.compile(r"^#\+LAST_MODIFIED:\s*(.*)$", re.M)
RE_SRC = re.compile(r"\bsrc:([^\s]+?)(?=[,\s=\])]|$)")
RE_CONTEXT_DESCRIPTION = re.compile(r"^:CONTEXT_DESCRIPTION:\s*(.*)$", re.M)
RE_CONTEXT_UPDATED = re.compile(r"^:CONTEXT_UPDATED:\s*\S+$", re.M)
RE_HEADING_START = re.compile(r"^\*+\s+\S+", re.M)
RE_PROP_END = re.compile(r"^:END:$", re.M)
RE_LINKED_ORG = re.compile(r"\[\[file:([^\]]+\.org)")
RE_TEST_CONTEXT = re.compile(r";;\s*TEST-CONTEXT:\s*(.*)")
RE_SRC_RAW = re.compile(r"src:([^\s]+?)(?=[,\s=\])]|$)")
RE_HEADING_WITH_ID = re.compile(r"^(\*+ +.*?)\n(?:.*?\n)*?^:PROPERTIES:\n(.*?)\n^:END:", re.M)
RE_ID_IN_PROPS = re.compile(r"^:ID:\s*(\S+)\s*$", re.M)

REQUIRED_HEADERS = ["#+TITLE:", "#+AUTHOR:", "#+DATE:", "#+LAST_MODIFIED:", "#+FILETAGS:"]

MON_PREFIXES = [
    "core",
    "core/prelude",
    "core/text",
    "how_to",
    "how_to/Term",
]

MON_ALIASES = {
    "ADT.mon": "AlgebraicDataTypes.mon",
    "layouts.mon": "Layouts.mon",
}

NON_FILE_SOURCES = frozenset({
    "user-request", "command-output", "user-report", "user-feedback",
    "user-observation", "llm-inference", "discussion",
})

SOURCE_EXTENSIONS = frozenset({
    ".c", ".h", ".py", ".mon", ".org", ".tsv", ".jsonl",
    ".js", ".css", ".html", ".frag", ".vert",
    ".ttf", ".png", ".pdf",
    ".yaml", ".yml", ".json", ".md", ".txt", ".sh",
    ".lua", ".toml", ".cfg", ".ini",
})

INFO_ZONE_EXEMPT_HEADERS = ["#+TITLE:", "#+AUTHOR:"]


def line_of(text: str, offset: int) -> int:
    return text[:offset].count("\n") + 1


def org_files(root: Path, skip_info: bool = False) -> list[Path]:
    files = sorted(p for p in root.rglob("*.org") if ".git" not in p.parts and not p.name.startswith(".#"))
    if skip_info:
        files = [p for p in files if "info/" not in str(p)]
    return files


def is_info_file(rel: str) -> bool:
    return rel.startswith("info/") or "/info/" in rel


def split_record_refs(raw: str) -> list[str]:
    out: list[str] = []
    for m in RE_RECORD_REF.finditer(raw):
        token = m.group(1).strip().strip(",.;")
        out.extend(x.strip().strip(",.;") for x in token.split(",") if x.strip())
    return out


def find_heading_ranges(text: str) -> list[tuple[int, int, str]]:
    ranges: list[tuple[int, int, str]] = []
    for m in RE_HEADING_START.finditer(text):
        start = m.start()
        heading_text = m.group()
        ranges.append((start, 0, heading_text))
    for i in range(len(ranges)):
        if i + 1 < len(ranges):
            ranges[i] = (ranges[i][0], ranges[i + 1][0], ranges[i][2])
        else:
            ranges[i] = (ranges[i][0], len(text), ranges[i][2])
    return ranges


def find_property_drawer(text: str, heading_offset: int) -> tuple[int, int] | None:
    after_heading = text[heading_offset:]
    lines = after_heading.split("\n")
    props_start = None
    props_end = None
    offset = heading_offset
    for li, line in enumerate(lines):
        if line.strip() == ":PROPERTIES:":
            props_start = offset
        elif line.strip() == ":END:" and props_start is not None:
            props_end = offset + len(line)
            break
        offset += len(line) + 1
    if props_start is not None and props_end is not None:
        return (props_start, props_end)
    return None


def get_heading_line(text: str, offset: int) -> int:
    return text[:offset].count("\n") + 1


def extract_src_references(text: str, rel: str) -> list[dict]:
    refs: list[dict] = []
    for m in RE_SRC.finditer(text):
        raw = m.group(1)
        file_part = raw.split(":")[0]
        refs.append({
            "file": rel,
            "line": line_of(text, m.start()),
            "raw": raw,
            "file_part": file_part,
        })
    return refs


def resolve_src_path(file_part: str, root: Path) -> Path | None:
    if "*" in file_part or file_part in NON_FILE_SOURCES:
        return None
    p = Path(file_part)
    if p.is_absolute():
        return p if p.exists() else None
    file_part_resolved = MON_ALIASES.get(file_part, file_part) if file_part.endswith(".mon") else file_part
    p = root / file_part_resolved
    if p.exists():
        return p
    p = root / "context" / file_part_resolved
    if p.exists():
        return p
    if file_part_resolved.endswith(".mon"):
        for prefix in MON_PREFIXES:
            p = root / prefix / file_part_resolved
            if p.exists():
                return p
    return None


def check_empty_headings(text: str, rel: str) -> list[tuple[str, int, str]]:
    if is_info_file(rel):
        return []
    issues: list[tuple[str, int, str]] = []
    ranges = find_heading_ranges(text)
    for start, end, heading_text in ranges:
        body = text[start:end]
        body_lines = body.split("\n")
        content_lines = [l for l in body_lines[1:] if l.strip() and not l.strip().startswith(":")]
        if not content_lines:
            line = get_heading_line(text, start)
            heading_clean = heading_text.strip()
            issues.append((rel, line, heading_clean))
    return issues


def check_descriptions(text: str, rel: str) -> list[tuple[str, int, str]]:
    if is_info_file(rel):
        return []
    issues: list[tuple[str, int, str]] = []
    ranges = find_heading_ranges(text)
    for start, end, heading_text in ranges:
        body = text[start:end]
        has_id = bool(RE_ID.search(body))
        if not has_id:
            continue
        has_desc = bool(RE_CONTEXT_DESCRIPTION.search(body))
        if not has_desc:
            line = get_heading_line(text, start)
            heading_clean = heading_text.strip()
            issues.append((rel, line, heading_clean))
    return issues


def check_src_refs(text: str, rel: str, root: Path) -> list[tuple[str, int, str, str]]:
    issues: list[tuple[str, int, str, str]] = []
    for m in RE_SRC.finditer(text):
        raw = m.group(1)
        file_part = raw.split(":")[0]
        if "*" in file_part or file_part in NON_FILE_SOURCES:
            continue
        if any(file_part.endswith(ext) for ext in SOURCE_EXTENSIONS):
            resolved = resolve_src_path(file_part, root)
            if resolved is None:
                line = line_of(text, m.start())
                issues.append((rel, line, raw, "FILE NOT FOUND"))
            elif resolved.is_dir():
                continue
            else:
                rest = raw[len(file_part):]
                if rest.startswith(":"):
                    parts = rest[1:].split("-", 1)
                    try:
                        line_no = int(parts[0])
                        total_lines = len(resolved.read_text(errors="replace").splitlines())
                        if line_no > total_lines:
                            issues.append((rel, line_of(text, m.start()), raw,
                                           f"LINE {line_no} EXCEEDS {total_lines}"))
                    except ValueError:
                        pass
        elif "/" in file_part:
            resolved = resolve_src_path(file_part, root)
            if resolved is None:
                line = line_of(text, m.start())
                issues.append((rel, line, raw, "FILE NOT FOUND"))
    return issues


def check_orphaned(files: list[Path], texts: list[tuple[Path, str, str]], root: Path) -> list[str]:
    all_org_names = {p.name for p in files}
    linked: set[str] = set()
    for path, rel, text in texts:
        if "index" in rel or "connections" in rel or "context" in rel:
            for m in RE_LINKED_ORG.finditer(text):
                target = m.group(1)
                target_name = Path(target).name
                if target_name in all_org_names:
                    linked.add(target_name)
    orphans = []
    for p in files:
        name = p.name
        if name in ("context.org", "README.org"):
            continue
        if name not in linked:
            orphans.append(str(p.relative_to(root)))
    return sorted(orphans)


def check_test_context_refs(root: Path, known_ids: set[str]) -> list[tuple[str, int, str]]:
    issues: list[tuple[str, int, str]] = []
    test_dir = root / "tests"
    if not test_dir.exists():
        return issues
    for test_file in sorted(test_dir.rglob("*")):
        if test_file.suffix not in (".mon", ".tsv"):
            continue
        rel_parts = test_file.relative_to(root).parts
        if any(part.startswith(".") or part == "__pycache__" for part in rel_parts):
            continue
        text = test_file.read_text(errors="replace")
        for m in RE_TEST_CONTEXT.finditer(text):
            context_ids = [cid.strip() for cid in m.group(1).split(",") if cid.strip()]
            for cid in context_ids:
                if cid.startswith("monadc.context.") and cid not in known_ids:
                    issues.append((str(test_file.relative_to(root)), line_of(text, m.start()), cid))
    return issues


def check_last_modified(text: str, rel: str, path: Path) -> list[tuple[str, int, str, str]]:
    issues: list[tuple[str, int, str, str]] = []
    m = RE_LAST_MODIFIED.search(text)
    if not m:
        return issues
    current_val = m.group(1).strip()
    try:
        stat_mtime = path.stat().st_mtime
    except OSError:
        return issues
    import datetime
    file_mtime = datetime.datetime.fromtimestamp(stat_mtime).strftime("%Y-%m-%d")
    if current_val.startswith("<") and current_val.endswith(">"):
        return issues
    found_date = None
    for candidate in re.findall(r"\d{4}-\d{2}-\d{2}", current_val):
        found_date = candidate
        break
    if found_date and found_date != file_mtime:
        issues.append((rel, line_of(text, m.start()), current_val, file_mdate))
    if found_date and found_date != file_mtime:
        issues.append((rel, line_of(text, m.start()), current_val, file_mtime))
    return issues


def fix_last_modified(text: str, path: Path, rel: str, root: Path) -> str | None:
    m = RE_LAST_MODIFIED.search(text)
    if not m:
        return None
    current = m.group(1).strip()
    today = date.today().strftime("%Y-%m-%d")
    if today in current:
        return None
    new_line = f"#+LAST_MODIFIED: {today}"
    new_text = text[:m.start()] + new_line + text[m.end():]
    return new_text


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="lint MonadC context org files")
    ap.add_argument("root", nargs="?", default="context", help="context directory")
    ap.add_argument("--strict-links", action="store_true", help="fail on local broken file links including repository-relative links")
    ap.add_argument("--check-record-refs", action="store_true", help="fail when record from:/supersedes:/blocks:/supports:/verifies: refs do not resolve to record or heading IDs")
    ap.add_argument("--json", action="store_true", help="emit a JSON summary instead of text")
    ap.add_argument("--skip-info", action="store_true", help="skip info/ directory (user-facing language reference)")
    ap.add_argument("--check-empty-headings", action="store_true", help="fail on headings with no body content")
    ap.add_argument("--check-description", action="store_true", help="fail on headings with :ID: but no :CONTEXT_DESCRIPTION:")
    ap.add_argument("--check-orphaned", action="store_true", help="fail on .org files not linked from index.org or connections.org")
    ap.add_argument("--check-src-refs", action="store_true", help="fail on src: references to non-existent files")
    ap.add_argument("--check-test-contexts", action="store_true", help="fail on TEST-CONTEXT references to unknown IDs")
    ap.add_argument("--fix-last-modified", action="store_true", help="update stale #+LAST_MODIFIED: values to today")
    ap.add_argument("--all", action="store_true", help="enable all checks")
    args = ap.parse_args(argv)

    if args.all:
        args.check_record_refs = True
        args.check_empty_headings = True
        args.check_description = True
        args.check_orphaned = True
        args.check_src_refs = True
        args.check_test_contexts = True

    root = Path(args.root).resolve()
    if root.name == "context":
        repo_root = root.parent
    else:
        repo_root = root

    files = org_files(root, skip_info=args.skip_info)
    ids: dict[str, list[tuple[str, int]]] = collections.defaultdict(list)
    records: dict[str, list[tuple[str, str, int]]] = collections.defaultdict(list)
    header_gaps: list[tuple[str, list[str]]] = []
    broken_links: list[tuple[str, int, str]] = []
    broken_id_links: list[tuple[str, int, str]] = []
    broken_record_refs: list[tuple[str, int, str]] = []
    file_rows: list[dict[str, object]] = []
    record_ref_uses: list[tuple[str, int, str]] = []
    empty_headings: list[tuple[str, int, str]] = []
    missing_descriptions: list[tuple[str, int, str]] = []
    broken_src_refs: list[tuple[str, int, str, str]] = []
    broken_test_contexts: list[tuple[str, int, str]] = []
    orphaned_files: list[str] = []
    stale_last_modified: list[tuple[str, int, str, str]] = []
    fixed_files: list[str] = []
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
        if is_info_file(rel):
            missing = [h for h in REQUIRED_HEADERS if h not in text and h not in INFO_ZONE_EXEMPT_HEADERS]
        else:
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

        if args.check_empty_headings:
            empty_headings.extend(check_empty_headings(text, rel))

        if args.check_description:
            missing_descriptions.extend(check_descriptions(text, rel))

        if args.check_src_refs:
            broken_src_refs.extend(check_src_refs(text, rel, repo_root))

    for rel, line, ref in record_ref_uses:
        if ":" in ref and not ref.startswith(("obs.", "inf.", "dec.", "doc.", "todo.", "think.", "idea.", "fix.", "monadc.context.")):
            continue
        if ref not in known_records and ref not in known_ids:
            broken_record_refs.append((rel, line, ref))

    if args.check_orphaned:
        orphaned_files = check_orphaned(files, texts, root)

    if args.check_test_contexts:
        broken_test_contexts = check_test_context_refs(repo_root, known_ids)

    if args.fix_last_modified:
        for path, rel, text in texts:
            new_text = fix_last_modified(text, path, rel, root)
            if new_text is not None:
                path.write_text(new_text)
                fixed_files.append(rel)

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

    if args.check_empty_headings:
        summary["empty_headings"] = empty_headings
    if args.check_description:
        summary["missing_descriptions"] = missing_descriptions
    if args.check_orphaned:
        summary["orphaned_files"] = orphaned_files
    if args.check_src_refs:
        summary["broken_src_refs"] = broken_src_refs
    if args.check_test_contexts:
        summary["broken_test_contexts"] = broken_test_contexts
    if args.fix_last_modified:
        summary["fixed_last_modified"] = fixed_files

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
        if args.check_empty_headings:
            print(f"empty headings: {len(empty_headings)}")
        if args.check_description:
            print(f"missing descriptions: {len(missing_descriptions)}")
        if args.check_orphaned:
            print(f"orphaned files: {len(orphaned_files)}")
        if args.check_src_refs:
            print(f"broken src refs: {len(broken_src_refs)}")
        if args.check_test_contexts:
            print(f"broken test contexts: {len(broken_test_contexts)}")
        if args.fix_last_modified:
            print(f"fixed last modified: {len(fixed_files)}")

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
        if args.check_empty_headings and empty_headings:
            print("\nEmpty headings (no body content):")
            for rel, line, heading in empty_headings:
                print(f"  {rel}:{line}: {heading}")
        if args.check_description and missing_descriptions:
            print("\nHeadings missing CONTEXT_DESCRIPTION:")
            for rel, line, heading in missing_descriptions:
                print(f"  {rel}:{line}: {heading}")
        if args.check_orphaned and orphaned_files:
            print("\nOrphaned files (not linked from index/connections):")
            for f in orphaned_files:
                print(f"  {f}")
        if args.check_src_refs and broken_src_refs:
            print("\nBroken src references:")
            for rel, line, raw, reason in broken_src_refs:
                print(f"  {rel}:{line}: src:{raw} - {reason}")
        if args.check_test_contexts and broken_test_contexts:
            print("\nBroken test context references:")
            for file, line, cid in broken_test_contexts:
                print(f"  {file}:{line}: TEST-CONTEXT {cid} - ID NOT FOUND")
        if args.fix_last_modified and fixed_files:
            print("\nFixed LAST_MODIFIED:")
            for f in fixed_files:
                print(f"  {f}")

    fail = bool(
        dup_ids or dup_records or header_gaps or broken_id_links
        or (args.strict_links and broken_links)
        or (args.check_record_refs and broken_record_refs)
        or (args.check_empty_headings and empty_headings)
        or (args.check_description and missing_descriptions)
        or (args.check_orphaned and orphaned_files)
        or (args.check_src_refs and broken_src_refs)
        or (args.check_test_contexts and broken_test_contexts)
    )
    return 1 if fail else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:] if len(sys.argv) > 1 else None))
