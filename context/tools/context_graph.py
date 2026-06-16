#!/usr/bin/env python3
"""Export a lightweight JSON graph from MonadC context org files."""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

RE_TITLE = re.compile(r"^#\+TITLE:\s*(.*)$", re.M)
RE_ID = re.compile(r"^:ID:\s*(\S+)\s*$", re.M)
RE_RECORD = re.compile(r"^\[(DOC|OBS|INF|DEC|TODO|THINK|IDEA|FIX)\s+id:([^\s\]]+)([^\]]*)\]", re.M)
RE_FILE_LINK = re.compile(r"\[\[file:([^\]\[]+?)(?:::?\*[^\]]*)?\](?:\[[^\]]*\])?\]")
RE_ID_LINK = re.compile(r"\[\[id:([^\]\[]+)\](?:\[[^\]]*\])?\]")
RE_RECORD_REF = re.compile(r"\b(from|supersedes|blocks|supports|verifies):([^\s\]]+)")

def line_of(text: str, off: int) -> int:
    return text[:off].count("\n") + 1

def org_files(root: Path):
    return sorted(p for p in root.rglob('*.org') if '.git' not in p.parts)

def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('context')
    nodes = []
    edges = []
    heading_ids = {}
    record_ids = {}
    for path in org_files(root):
        rel = str(path.relative_to(root))
        text = path.read_text(errors='replace')
        title = RE_TITLE.search(text)
        nodes.append({'id': f'file:{rel}', 'kind': 'file', 'label': title.group(1).strip() if title else rel, 'path': rel})
        for m in RE_ID.finditer(text):
            hid = m.group(1)
            heading_ids[hid] = {'file': rel, 'line': line_of(text, m.start())}
            nodes.append({'id': hid, 'kind': 'heading', 'label': hid, 'path': rel, 'line': line_of(text, m.start())})
            edges.append({'from': f'file:{rel}', 'to': hid, 'kind': 'contains'})
        for m in RE_RECORD.finditer(text):
            typ, rid, rest = m.group(1), m.group(2), m.group(3)
            record_ids[rid] = {'file': rel, 'line': line_of(text, m.start()), 'type': typ}
            nodes.append({'id': rid, 'kind': 'record', 'record_type': typ, 'label': rid, 'path': rel, 'line': line_of(text, m.start())})
            edges.append({'from': f'file:{rel}', 'to': rid, 'kind': 'contains'})
            for rm in RE_RECORD_REF.finditer(rest):
                kind = rm.group(1)
                for target in [x.strip().strip(',.;') for x in rm.group(2).split(',') if x.strip()]:
                    edges.append({'from': rid, 'to': target, 'kind': kind, 'status': 'unresolved'})
        for m in RE_FILE_LINK.finditer(text):
            raw = m.group(1).split('::',1)[0]
            if raw and not raw.startswith(('http:', 'https:', '/')):
                edges.append({'from': f'file:{rel}', 'to': f'file:{raw}', 'kind': 'file-link', 'line': line_of(text, m.start())})
        for m in RE_ID_LINK.finditer(text):
            edges.append({'from': f'file:{rel}', 'to': m.group(1).strip(), 'kind': 'id-link', 'line': line_of(text, m.start())})
    known = {n['id'] for n in nodes}
    for e in edges:
        if e.get('status') == 'unresolved' and e['to'] in known:
            e['status'] = 'resolved'
    print(json.dumps({'nodes': nodes, 'edges': edges, 'counts': {'nodes': len(nodes), 'edges': len(edges)}}, indent=2, sort_keys=True))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
