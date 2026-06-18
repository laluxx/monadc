#!/usr/bin/env python3
"""Validate every reference in context/*.org — source anchors, cross-links, IDs, records.

Stale anchors are test failures. If it fails, the documentation is stale;
if it passes, the references are true by test.
"""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CONTEXT = ROOT / "context"

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
    "...",
})

SOURCE_EXTENSIONS = frozenset({
    ".c", ".h", ".py", ".mon", ".org", ".tsv", ".jsonl",
    ".js", ".css", ".html", ".frag", ".vert",
    ".ttf", ".png", ".pdf",
    ".yaml", ".yml", ".json", ".md", ".txt", ".sh",
    ".lua", ".toml", ".cfg", ".ini",
})

SRC_PATTERN = re.compile(r'src:([^\s]+?)(?=[,\s=]|$|\))')
LINK_PATTERN = re.compile(r'\[\[file:([^\]]+?\.org)(?:::[^\]]*?)?\]')
RECORD_PATTERN = re.compile(
    r'\[('
    r'DOC|OBS|INF|DEC|TODO|THINK|IDEA|FIX'
    r')\s+'
    r'id:(\S+)'
    r'(?:\s+src:(\S+?))?'
    r'(?:\s+from:(\S+?))?'
    r'(?:\s+conf:(\S+?))?'
    r'(?:\s+date:(\S+?))?'
    r'(?:\s+by:(\S+?))?'
    r'(?:\s+status:(\S+?))?'
    r'(?:\s+owner:(\S+?))?'
    r'(?:\s+author:(\S+?))?'
    r'\]'
)
ID_PATTERN = re.compile(r'[:=]ID:\s+monadc\.context\.(\S+)')


def collect_org_files() -> list[Path]:
    return sorted(CONTEXT.glob("*.org"))


def file_line_count(path: Path) -> int:
    try:
        with open(path, encoding="utf-8") as fh:
            for count, _line in enumerate(fh, 1):
                pass
        return count
    except OSError:
        return 0


def has_source_extension(path_str: str) -> bool:
    return any(path_str.endswith(ext) for ext in SOURCE_EXTENSIONS)


def resolve_path(file_part: str) -> Path | None:
    if "*" in file_part:
        return None
    p = Path(file_part)
    if p.is_absolute():
        return p if p.exists() else None
    file_part_resolved = MON_ALIASES.get(file_part, file_part) if file_part.endswith(".mon") else file_part
    p = ROOT / file_part_resolved
    if p.exists():
        return p
    p = CONTEXT / file_part_resolved
    if p.exists():
        return p
    if file_part_resolved.endswith(".mon"):
        for prefix in MON_PREFIXES:
            p = ROOT / prefix / file_part_resolved
            if p.exists():
                return p
    return None


def parse_src_value(raw: str):
    raw = raw.strip()
    raw = raw.rstrip("=)")
    raw = raw.lstrip("=(")
    range_match = re.match(r'^(.+?):(\d+)-(\d+)$', raw)
    if range_match:
        return range_match.group(1), int(range_match.group(2)), int(range_match.group(3))
    col_match = re.match(r'^(.+?):(\d+):(\d+)$', raw)
    if col_match:
        return col_match.group(1), int(col_match.group(2)), None
    line_match = re.match(r'^(.+?):(\d+)$', raw)
    if line_match:
        return line_match.group(1), int(line_match.group(2)), None
    return raw, None, None


def extract_src_references(filepath: Path) -> list[dict]:
    refs = []
    text = filepath.read_text(encoding="utf-8")
    lines = text.splitlines()
    for line_no, line in enumerate(lines, 1):
        for match in SRC_PATTERN.finditer(line):
            raw_value = match.group(1)
            parts = [p.strip() for p in raw_value.split(",")]
            for part in parts:
                if not part:
                    continue
                file_part, line_start, line_end = parse_src_value(part)
                if file_part in NON_FILE_SOURCES:
                    continue
                if "*" in file_part:
                    continue
                if not has_source_extension(file_part):
                    resolved = resolve_path(file_part)
                    if resolved is None:
                        if "/" not in file_part and "." not in file_part:
                            continue
                resolved = resolve_path(file_part)
                refs.append({
                    "context_file": str(filepath.relative_to(ROOT)),
                    "context_line": line_no,
                    "raw": part,
                    "file_part": file_part,
                    "line_start": line_start,
                    "line_end": line_end,
                    "resolved": resolved,
                })
    return refs


def extract_org_links(filepath: Path) -> list[dict]:
    links = []
    text = filepath.read_text(encoding="utf-8")
    lines = text.splitlines()
    for line_no, line in enumerate(lines, 1):
        for match in LINK_PATTERN.finditer(line):
            target = match.group(1)
            links.append({
                "context_file": str(filepath.relative_to(ROOT)),
                "context_line": line_no,
                "target": target,
            })
    return links


def extract_all_ids(filepath: Path) -> list[dict]:
    ids = []
    text = filepath.read_text(encoding="utf-8")
    lines = text.splitlines()
    for line_no, line in enumerate(lines, 1):
        for match in ID_PATTERN.finditer(line):
            ids.append({
                "context_file": str(filepath.relative_to(ROOT)),
                "context_line": line_no,
                "id": f"monadc.context.{match.group(1)}",
            })
    return ids


def extract_records(filepath: Path) -> list[dict]:
    records = []
    text = filepath.read_text(encoding="utf-8")
    lines = text.splitlines()
    for line_no, line in enumerate(lines, 1):
        for match in RECORD_PATTERN.finditer(line):
            kind = match.group(1)
            props = {
                "id": match.group(2),
                "src": match.group(3),
                "from": match.group(4),
                "conf": match.group(5),
                "date": match.group(6),
                "by": match.group(7),
                "status": match.group(8),
                "owner": match.group(9),
                "author": match.group(10),
            }
            records.append({
                "context_file": str(filepath.relative_to(ROOT)),
                "context_line": line_no,
                "kind": kind,
                "props": props,
            })
    return records




def full_repo_sources_available() -> bool:
    return any((ROOT / name).exists() for name in ("reader.c", "wisp.c", "codegen.c", "monad"))


def extract_test_context_refs() -> list[dict]:
    refs: list[dict] = []
    metadata_pattern = re.compile(r';;\s*(TEST-CONTEXT):\s*(.*)')
    for test_file in sorted((ROOT / "tests").rglob("*.mon")):
        rel_parts = test_file.relative_to(ROOT).parts
        if any(part.startswith(".") or part == "__pycache__" for part in rel_parts):
            continue
        for line_no, line in enumerate(test_file.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if not line.startswith(";;"):
                if line.strip():
                    break
                continue
            match = metadata_pattern.match(line)
            if not match:
                continue
            for context_id in [part.strip() for part in match.group(2).split(',') if part.strip()]:
                refs.append({
                    "test_file": str(test_file.relative_to(ROOT)),
                    "test_line": line_no,
                    "context_id": context_id,
                })
    corpus_prefixes = {
        "reader-refinements.tsv": "refinement",
        "reader-interactions.tsv": "interaction",
        "reader-supersets.tsv": "superset",
        "reader-atoms.tsv": "language",
        "reader-sugars.tsv": "sugar",
    }
    for corpus_name in corpus_prefixes:
        corpus = ROOT / "tests" / corpus_name
        if not corpus.exists():
            continue
        for line_no, line in enumerate(corpus.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if not line or line.startswith('#'):
                continue
            fields = line.split('\t')
            if len(fields) < 2:
                continue
            for context_id in [part.strip() for part in fields[1].split(',') if part.strip()]:
                refs.append({
                    "test_file": str(corpus.relative_to(ROOT)),
                    "test_line": line_no,
                    "context_id": context_id,
                })
    return refs


class TestContextSourceReferences(unittest.TestCase):
    """Every src: reference in context/*.org must resolve to a real file."""

    maxDiff = None

    def test_all_src_references_resolve(self):
        if not full_repo_sources_available():
            raise unittest.SkipTest("full compiler checkout is not present; src: file validation runs in the repository")
        org_files = collect_org_files()
        self.assertGreater(len(org_files), 0, "No .org files found in context/")
        all_refs: list[dict] = []
        failed_refs: list[str] = []
        for org_file in org_files:
            refs = extract_src_references(org_file)
            for ref in refs:
                all_refs.append(ref)
                if ref["resolved"] is None:
                    failed_refs.append(
                        f"{ref['context_file']}:{ref['context_line']}: "
                        f"src:{ref['raw']} — FILE NOT FOUND"
                    )
                    continue
                resolved = ref["resolved"]
                if resolved.is_dir():
                    continue
                if ref["line_start"] is not None:
                    total_lines = file_line_count(resolved)
                    if ref["line_start"] > total_lines:
                        failed_refs.append(
                            f"{ref['context_file']}:{ref['context_line']}: "
                            f"src:{ref['raw']} — LINE {ref['line_start']} EXCEEDS "
                            f"{resolved.relative_to(ROOT)} line count ({total_lines})"
                        )
                if ref["line_end"] is not None:
                    total_lines = file_line_count(resolved)
                    if ref["line_end"] > total_lines:
                        failed_refs.append(
                            f"{ref['context_file']}:{ref['context_line']}: "
                            f"src:{ref['raw']} — END LINE {ref['line_end']} EXCEEDS "
                            f"{resolved.relative_to(ROOT)} line count ({total_lines})"
                        )
        total = len(all_refs)
        failed = len(failed_refs)
        summary = [
            f"\n1. Source reference validation:",
            f"   Files checked: {len(org_files)}",
            f"   References found: {total}",
            f"   References failed: {failed}",
        ]
        if failed_refs:
            summary.append("   Failed references:")
            for r in failed_refs:
                summary.append(f"     \u2717 {r}")
        print("\n".join(summary))
        self.assertEqual(failed, 0, f"{failed}/{total} src: references failed\n" + "\n".join(failed_refs))

    def test_all_org_links_resolve(self):
        """Every [[file:*.org]] cross-reference must point to a real context file."""
        org_files = collect_org_files()
        existing = {f.name for f in org_files}
        all_links: list[dict] = []
        failed_links: list[str] = []
        for org_file in org_files:
            links = extract_org_links(org_file)
            for link in links:
                all_links.append(link)
                target = link["target"]
                target_path = (ROOT / link["context_file"]).parent.joinpath(target).resolve()
                if target in existing or target_path.exists():
                    continue
                # The distributed tests/context snapshot may omit ../info/*.org.
                # Validate those links in a full checkout, but do not fail the
                # standalone test tar merely because the external manual was not
                # packaged with it.
                if target.startswith("../") and not full_repo_sources_available():
                    continue
                failed_links.append(
                    f"{link['context_file']}:{link['context_line']}: "
                    f"[[file:{target}]] — DOES NOT EXIST"
                )
        total = len(all_links)
        failed = len(failed_links)
        summary = [
            f"\n2. Org cross-link validation:",
            f"   Files checked: {len(org_files)}",
            f"   Links found: {total}",
            f"   Links failed: {failed}",
        ]
        if failed_links:
            summary.append("   Failed links:")
            for r in failed_links:
                summary.append(f"     \u2717 {r}")
        print("\n".join(summary))
        self.assertEqual(failed, 0, f"{failed}/{total} [[file:*.org]] links failed\n" + "\n".join(failed_links))

    def test_all_ids_are_unique(self):
        """Every monadc.context.* ID must be unique across all context files."""
        org_files = collect_org_files()
        seen: dict[str, list[tuple[str, int]]] = {}
        for org_file in org_files:
            ids = extract_all_ids(org_file)
            for entry in ids:
                id_key = entry["id"]
                loc = (entry["context_file"], entry["context_line"])
                if id_key in seen:
                    seen[id_key].append(loc)
                else:
                    seen[id_key] = [loc]
        duplicates = {k: v for k, v in seen.items() if len(v) > 1}
        total_ids = len(seen)
        dup_count = len(duplicates)
        summary = [
            f"\n3. ID uniqueness check:",
            f"   Files checked: {len(org_files)}",
            f"   IDs found: {total_ids}",
            f"   Duplicate IDs: {dup_count}",
        ]
        if duplicates:
            summary.append("   Duplicate IDs:")
            for id_key, locs in sorted(duplicates.items()):
                loc_strs = [f"{f}:{l}" for f, l in locs]
                summary.append(f"     \u2717 {id_key} appears {len(locs)}x: {', '.join(loc_strs)}")
        print("\n".join(summary))
        self.assertEqual(dup_count, 0, f"{dup_count} duplicate IDs found\n" + str(duplicates))

    def test_obs_records_have_required_props(self):
        """Every OBS record must have id:, src:, and conf: properties."""
        org_files = collect_org_files()
        all_obs: list[dict] = []
        failed: list[str] = []
        for org_file in org_files:
            records = extract_records(org_file)
            for rec in records:
                if rec["kind"] != "OBS":
                    continue
                all_obs.append(rec)
                missing = []
                if not rec["props"]["id"]:
                    missing.append("id")
                if not rec["props"]["src"]:
                    missing.append("src")
                if not rec["props"]["conf"]:
                    missing.append("conf")
                if missing:
                    failed.append(
                        f"{rec['context_file']}:{rec['context_line']}: "
                        f"OBS id:{rec['props']['id']} — missing: {', '.join(missing)}"
                    )
        total = len(all_obs)
        failed_count = len(failed)
        summary = [
            f"\n4. OBS record property completeness:",
            f"   Files checked: {len(org_files)}",
            f"   OBS records found: {total}",
            f"   Records with missing props: {failed_count}",
        ]
        if failed:
            summary.append("   Failed records:")
            for r in failed:
                summary.append(f"     \u2717 {r}")
        print("\n".join(summary))
        self.assertEqual(failed_count, 0, f"{failed_count} OBS records missing required props\n" + "\n".join(failed))

    def test_inf_records_have_required_props(self):
        """Every INF record must have id:, from:, and conf: properties."""
        org_files = collect_org_files()
        all_inf: list[dict] = []
        failed: list[str] = []
        for org_file in org_files:
            records = extract_records(org_file)
            for rec in records:
                if rec["kind"] != "INF":
                    continue
                all_inf.append(rec)
                missing = []
                if not rec["props"]["id"]:
                    missing.append("id")
                if not rec["props"]["from"]:
                    missing.append("from")
                if not rec["props"]["conf"]:
                    missing.append("conf")
                if missing:
                    failed.append(
                        f"{rec['context_file']}:{rec['context_line']}: "
                        f"INF id:{rec['props']['id']} — missing: {', '.join(missing)}"
                    )
        total = len(all_inf)
        failed_count = len(failed)
        summary = [
            f"\n5. INF record property completeness:",
            f"   Files checked: {len(org_files)}",
            f"   INF records found: {total}",
            f"   Records with missing props: {failed_count}",
        ]
        if failed:
            summary.append("   Failed records:")
            for r in failed:
                summary.append(f"     \u2717 {r}")
        print("\n".join(summary))
        self.assertEqual(failed_count, 0, f"{failed_count} INF records missing required props\n" + "\n".join(failed))

    def test_all_test_context_links_resolve(self):
        """Every TEST-CONTEXT reference must point to a real monadc.context.* ID."""
        known_ids = {entry["id"] for org_file in collect_org_files() for entry in extract_all_ids(org_file)}
        refs = extract_test_context_refs()
        failed: list[str] = []
        for ref in refs:
            context_id = ref["context_id"]
            if context_id.startswith("monadc.context.") and context_id not in known_ids:
                failed.append(
                    f"{ref['test_file']}:{ref['test_line']}: "
                    f"TEST-CONTEXT {context_id} — ID NOT FOUND"
                )
        summary = [
            f"\n6. Test-to-context link validation:",
            f"   Test context links found: {len(refs)}",
            f"   Links failed: {len(failed)}",
        ]
        if failed:
            summary.append("   Failed links:")
            for line in failed[:200]:
                summary.append(f"     ✗ {line}")
            if len(failed) > 200:
                summary.append(f"     … {len(failed) - 200} more")
        print("\n".join(summary))
        self.assertEqual(len(failed), 0, f"{len(failed)}/{len(refs)} TEST-CONTEXT links failed\n" + "\n".join(failed[:200]))


if __name__ == "__main__":
    unittest.main()
