#!/usr/bin/env python3

from __future__ import annotations

import json
import mimetypes
import os
import re
import shlex
import shutil
import subprocess
import sys
import webbrowser
from dataclasses import asdict, dataclass
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse


ROOT = Path(__file__).resolve().parent
VISUALIZER = ROOT / "context" / "visualizer"
PERF_RESULTS: list[dict[str, object]] = []


@dataclass
class Node:
    id: str
    label: str
    kind: str
    file: str
    summary: str
    content: str = ""
    heading: str = ""
    record_type: str = ""
    source: str = ""
    confidence: str = ""
    status: str = ""
    mutable: bool = False
    line: int = 0
    completed_at: str = ""
    raw_content: str = ""


@dataclass
class Edge:
    source: str
    target: str
    label: str
    kind: str


@dataclass
class Method:
    id: str
    name: str
    signature: str
    group: str
    file: str
    line: int
    form: str
    uses: list[str]
    used_by: list[str]


def parse_context_graph() -> dict[str, object]:
    nodes: dict[str, Node] = {}
    edges: list[Edge] = []
    for path in sorted((ROOT / "context").glob("*.org")):
        parse_org_file(path, nodes, edges)
    for path in sorted(p for p in (ROOT / "tests").rglob("*.mon") if not p.name.startswith(".")):
        parse_test_fixture(path, nodes, edges)
    for corpus_path in [
        ROOT / "tests" / "language_corpus.tsv",
        ROOT / "tests" / "reader-refinements.tsv",
        ROOT / "tests" / "reader-interactions.tsv",
        ROOT / "tests" / "reader-supersets.tsv",
        ROOT / "tests" / "reader-atoms.tsv",
        ROOT / "tests" / "reader-sugars.tsv",
    ]:
        if corpus_path.exists():
            parse_language_corpus(corpus_path, nodes, edges)
    for path in sorted((ROOT / "tests").rglob("*.org")):
        parse_org_file(path, nodes, edges)

    stats = {
        "objects": len(nodes),
        "morphisms": len(edges),
        "todos": sum(1 for node in nodes.values() if node.kind == "todo"),
        "openTodos": sum(1 for node in nodes.values() if node.kind == "todo" and node.status not in {"done", "closed", "DONE"}),
        "tests": sum(1 for node in nodes.values() if node.kind == "test"),
        "decisions": sum(1 for node in nodes.values() if node.kind == "decision"),
        "documentation": sum(1 for node in nodes.values() if node.kind == "documentation"),
    }
    return {
        "nodes": [asdict(n) for n in nodes.values()],
        "edges": [asdict(e) for e in edges],
        "methods": [asdict(m) for m in parse_core_methods()],
        "stats": stats,
    }


def parse_core_methods() -> list[Method]:
    records: list[tuple[Method, str]] = []
    for path in sorted(p for p in (ROOT / "core").rglob("*.mon") if not p.name.startswith(".")):
        rel = path.relative_to(ROOT).as_posix()
        group = "prelude" if rel.startswith("core/prelude/") else "core"
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        starts: list[tuple[int, str, str, str]] = []
        for line_no, raw in enumerate(lines, 1):
            line = raw.strip()
            if not line or line.startswith(";;"):
                continue

            match = re.match(r"^\(define\s+\(([^()\s]+)\s+([^)]*->[^)]*)\)", line)
            if match:
                starts.append((line_no, match.group(1), compact_signature(match.group(2)), "function"))
                continue

            match = re.match(r"^\(define\s+\[([^\]\s]+)\s+(?:::|:)\s+([^\]]+)\]", line)
            if match:
                starts.append((line_no, match.group(1), compact_signature(match.group(2)), "value"))
                continue

            match = re.match(r"^define\s+([^\s]+)\s+::\s+(.+)$", line)
            if match:
                starts.append((line_no, match.group(1), compact_signature(match.group(2)), "function"))
                continue

        for index, (line_no, name, signature, form) in enumerate(starts):
            end_line = starts[index + 1][0] - 1 if index + 1 < len(starts) else len(lines)
            body = "\n".join(lines[line_no - 1:end_line])
            method_id = f"method:{group}:{rel}:{line_no}:{name}"
            records.append((Method(method_id, name, signature, group, rel, line_no, form, [], []), body))

    by_name: dict[str, list[Method]] = {}
    for method, _body in records:
        by_name.setdefault(method.name, []).append(method)

    call_pattern = re.compile(r"\(([^\s()[\]{};\"']+)")
    for method, body in records:
        called_names = {
            token
            for token in call_pattern.findall(strip_line_comments(body))
            if token in by_name and token != method.name
        }
        method.uses = sorted(called_names)
        for name in method.uses:
            for target in by_name[name]:
                if method.name not in target.used_by:
                    target.used_by.append(method.name)

    for method, _body in records:
        method.used_by.sort()
    return [method for method, _body in records]


def strip_line_comments(value: str) -> str:
    return "\n".join(line.split(";;", 1)[0] for line in value.splitlines())


def compact_signature(value: str) -> str:
    value = re.sub(r"\s+", " ", value).strip()
    value = re.sub(r"\[[^\]\s]+\s*(?:::|:)\s*([^\]]+)\]", r"\1", value)
    return re.sub(r"\s+", " ", value).strip()


def parse_org_file(path: Path, nodes: dict[str, Node], edges: list[Edge]) -> None:
    rel = path.relative_to(ROOT).as_posix()
    text = path.read_text(encoding="utf-8", errors="replace")
    file_id = f"file:{rel}"
    lines = text.splitlines()
    nodes.setdefault(file_id, Node(file_id, path.stem, "file", rel, first_sentence(text), text[:2000], raw_content=text))
    current_heading = file_id
    pending_heading = ""
    pending_heading_line = 1
    pending_todo = False
    drawer: dict[str, str] = {}
    in_drawer = False

    for line_no, line in enumerate(lines, 1):
        heading = re.match(r"^(\*+)\s+(?:(TODO|ACTIVE|VERIFY|WAIT|DONE|BLOCKED|SUPERSEDED)\s+)?(.+)", line)
        if heading:
            pending_heading = heading.group(3).strip()
            pending_heading_line = line_no
            pending_todo = heading.group(2) == "TODO"
            if heading.group(2) in {"TODO", "DONE", "BLOCKED", "WAIT", "VERIFY"}:
                node_id = f"todo:{rel}:{line_no}"
                status = heading.group(2).lower()
                body = org_section_body(lines, pending_heading_line)
                raw_body = raw_org_section_body(lines, pending_heading_line)
                nodes[node_id] = Node(
                    node_id,
                    pending_heading,
                    "todo",
                    rel,
                    first_sentence(body),
                    body,
                    pending_heading,
                    "TODO",
                    "",
                    "",
                    status=status,
                    mutable=True,
                    line=line_no,
                    raw_content=raw_body,
                )
                edges.append(Edge(file_id, node_id, "contains", "contains"))
                current_heading = node_id
            drawer = {}
            in_drawer = False
            continue
        if line.strip() == ":PROPERTIES:":
            in_drawer = True
            continue
        closed = re.match(r"^CLOSED:\s+\[([^\]]+)\]", line.strip())
        if closed and current_heading in nodes:
            nodes[current_heading].completed_at = closed.group(1)
            continue
        if in_drawer and line.strip() == ":END:":
            in_drawer = False
            node_id = drawer.get("ID")
            if node_id:
                kind = normalize_kind(drawer.get("CONTEXT_KIND", "todo" if pending_todo else "context"))
                body = org_section_body(lines, pending_heading_line)
                raw_body = raw_org_section_body(lines, pending_heading_line)
                nodes[node_id] = Node(
                    node_id,
                    pending_heading or node_id,
                    kind,
                    rel,
                    first_sentence(body),
                    body,
                    pending_heading,
                    "",
                    drawer.get("SOURCE", ""),
                    drawer.get("CONFIDENCE", ""),
                    drawer.get("CONTEXT_STATUS", "open" if pending_todo else ""),
                    pending_todo,
                    line_no,
                    raw_content=raw_body,
                )
                edges.append(Edge(file_id, node_id, "contains", "contains"))
                current_heading = node_id
            continue
        if in_drawer:
            prop = re.match(r"^:([^:]+):\s*(.*)", line)
            if prop:
                drawer[prop.group(1)] = prop.group(2)
            continue

        record = re.match(r"\[(OBS|INF|DEC|TODO|DOC|THINK|FIX|IDEA)\s+([^\]]+)\]\s*(.*)", line)
        if record:
            attrs = parse_attrs(record.group(2))
            rec_id = attrs.get("id", f"{rel}:{len(nodes)}")
            record_type = record.group(1)
            kind = {"OBS": "observation", "INF": "inference", "DEC": "decision", "TODO": "todo", "DOC": "documentation", "THINK": "think", "FIX": "fix", "IDEA": "idea"}[record_type]
            body = record_body(lines, line_no)
            raw_body = raw_record_body(lines, line_no)
            summary = record.group(3).strip() or first_sentence(body)
            nodes[rec_id] = Node(
                rec_id,
                rec_id,
                kind,
                rel,
                summary,
                body,
                pending_heading,
                record_type,
                attrs.get("src", ""),
                attrs.get("conf", ""),
                attrs.get("status", ""),
                kind == "todo",
                line_no,
                raw_content=raw_body,
            )
            edges.append(Edge(current_heading, rec_id, "states", "states"))
            for source in split_refs(attrs.get("from", "")):
                edges.append(Edge(source, rec_id, "supports", "supports"))
            src = attrs.get("src")
            if src:
                src_id = f"source:{src}"
                nodes.setdefault(src_id, Node(src_id, src, "source", src, "source reference", raw_content=src))
                edges.append(Edge(rec_id, src_id, "evidenced by", "evidence"))
            continue

        for target in re.findall(r"\[\[file:([^]\n]+?)(?:\][^]\n]*)?\]\]", line):
            target_file = target.split("::", 1)[0].split("#", 1)[0]
            try:
                target_id = f"file:{(path.parent / target_file).resolve().relative_to(ROOT).as_posix()}"
                edges.append(Edge(current_heading, target_id, "links to", "link"))
            except ValueError:
                pass


def parse_test_fixture(path: Path, nodes: dict[str, Node], edges: list[Edge]) -> None:
    rel = path.relative_to(ROOT).as_posix()
    metadata: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r";;\s*(TEST-[A-Z-]+):\s*(.*)", line)
        if match:
            metadata[match.group(1)] = match.group(2).strip()
        elif line.strip():
            break
    test_id = metadata.get("TEST-ID")
    if not test_id:
        return
    nodes[test_id] = Node(test_id, test_id.removeprefix("tests."), "test", rel, metadata.get("TEST-PURPOSE", ""), raw_content=path.read_text(encoding="utf-8", errors="replace"))
    for context_id in split_refs(metadata.get("TEST-CONTEXT", "")):
        edges.append(Edge(test_id, context_id, "verifies", "verifies"))


def parse_language_corpus(path: Path, nodes: dict[str, Node], edges: list[Edge]) -> None:
    rel = path.relative_to(ROOT).as_posix()
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) < 3:
            continue
        name, context_id, purpose = fields[0], fields[1], fields[2]
        prefix = {
            "reader-sugars.tsv": "sugar",
            "reader-refinements.tsv": "refinement",
            "reader-interactions.tsv": "interaction",
            "reader-supersets.tsv": "superset",
        }.get(path.name, "language")
        test_id = f"tests.{prefix}.{name}"
        source = fields[3] if len(fields) > 3 else ""
        nodes[test_id] = Node(test_id, f"{prefix}.{name}", "test", rel, purpose, source, raw_content=line)
        edges.append(Edge(test_id, context_id, "verifies", "verifies"))


def parse_attrs(value: str) -> dict[str, str]:
    return {key: val for key, val in re.findall(r"([A-Za-z_:-]+):([^ ]+)", value)}


def split_refs(value: str) -> list[str]:
    return [part.strip() for part in value.split(",") if part.strip()]


def normalize_kind(kind: str) -> str:
    if "doc" in kind or "documentation" in kind:
        return "documentation"
    if "decision" in kind:
        return "decision"
    if "test" in kind:
        return "test-meta"
    if "task" in kind or kind == "todo":
        return "todo"
    if "metadata" in kind:
        return "metadata"
    return kind or "context"


def first_sentence(text: str) -> str:
    for line in text.splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith("#+") and not stripped.startswith("*") and stripped != ":PROPERTIES:":
            return strip_org_markup(stripped)[:220]
    return ""


def org_section_body(lines: list[str], heading_line: int) -> str:
    start = heading_line
    while start < len(lines) and lines[start].strip() != ":END:":
        start += 1
    if start < len(lines):
        start += 1
    end = start
    while end < len(lines) and not re.match(r"^\*+\s+", lines[end]):
        end += 1
    return strip_org_markup("\n".join(lines[start:end]).strip())


def raw_org_section_body(lines: list[str], heading_line: int) -> str:
    start = max(0, heading_line - 1)
    end = start + 1
    while end < len(lines) and not re.match(r"^\*+\s+", lines[end]):
        end += 1
    return "\n".join(lines[start:end]).strip()


def record_body(lines: list[str], record_line: int) -> str:
    body: list[str] = []
    for line in lines[record_line:]:
        if re.match(r"^\[(OBS|INF|DEC|TODO|DOC|THINK|FIX|IDEA)\s+", line) or re.match(r"^\*+\s+", line):
            break
        if line.strip() == ":PROPERTIES:":
            break
        body.append(line)
    return strip_org_markup("\n".join(body).strip())


def raw_record_body(lines: list[str], record_line: int) -> str:
    start = max(0, record_line - 1)
    end = record_line
    while end < len(lines):
        if end > start and (re.match(r"^\[(OBS|INF|DEC|TODO|DOC|THINK|FIX|IDEA)\s+", lines[end]) or re.match(r"^\*+\s+", lines[end])):
            break
        if end > start and lines[end].strip() == ":PROPERTIES:":
            break
        end += 1
    return "\n".join(lines[start:end]).strip()


def strip_org_markup(value: str) -> str:
    value = re.sub(r"=([^=\n]+)=", r"\1", value)
    value = re.sub(r"\[\[file:([^]\n]+?)(?:\][^]\n]*)?\]\]", r"\1", value)
    return value


class Handler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        path = unquote(urlparse(self.path).path)
        if path == "/api/context":
            self.send_json(parse_context_graph())
            return
        if path == "/api/perf":
            self.send_json(PERF_RESULTS[-1] if PERF_RESULTS else {})
            return
        if path == "/":
            path = "/context/visualizer/index.html"
        file_path = (ROOT / path.lstrip("/")).resolve()
        if not file_path.is_file() or ROOT not in file_path.parents:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", mimetypes.guess_type(file_path)[0] or "application/octet-stream")
        self.end_headers()
        self.wfile.write(file_path.read_bytes())

    def do_POST(self) -> None:
        path = unquote(urlparse(self.path).path)
        if path not in {"/api/todo", "/api/open", "/api/perf"}:
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", "0"))
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            if path == "/api/todo":
                update_todo_status(str(payload["id"]), str(payload["status"]))
                self.send_json(parse_context_graph())
            elif path == "/api/open":
                open_file_in_editor(str(payload["file"]), int(payload.get("line") or 1))
                self.send_json({"ok": True})
            else:
                PERF_RESULTS.append(payload)
                del PERF_RESULTS[:-10]
                self.send_json({"ok": True})
        except Exception as error:
            self.send_error(400, str(error))
            return

    def send_json(self, payload: object) -> None:
        data = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, _format: str, *_args: object) -> None:
        return


def update_todo_status(node_id: str, status: str) -> None:
    normalized = status.lower()
    if normalized not in {"open", "done", "blocked"}:
        raise ValueError("status must be open, done, or blocked")

    if node_id.startswith("todo:"):
        _prefix, rel, line_text = node_id.split(":", 2)
        path = (ROOT / rel).resolve()
        line_no = int(line_text)
        lines = path.read_text(encoding="utf-8").splitlines()
        idx = line_no - 1
        lines = rewrite_todo_heading(lines, idx, normalized, org_timestamp())
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return

    for path in sorted((ROOT / "context").glob("*.org")):
        lines = path.read_text(encoding="utf-8").splitlines()
        changed = False
        for idx, line in enumerate(lines):
            if not line.startswith("[TODO ") or f"id:{node_id}" not in line:
                continue
            if "status:" in line:
                lines[idx] = re.sub(r"status:[^\] ]+", f"status:{normalized}", line)
            else:
                lines[idx] = line.replace("]", f" status:{normalized}]", 1)
            changed = True
            break
        if changed:
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            return
    raise ValueError(f"TODO not found: {node_id}")


def rewrite_todo_heading(lines: list[str], idx: int, status: str, timestamp: str) -> list[str]:
    keyword = {"open": "TODO", "done": "DONE", "blocked": "BLOCKED"}[status]
    rewritten = list(lines)
    rewritten[idx] = re.sub(r"^(\*+\s+)(TODO|DONE|BLOCKED|WAIT|VERIFY|ACTIVE)\s+", rf"\1{keyword} ", rewritten[idx])
    closed_idx = idx + 1
    has_closed = closed_idx < len(rewritten) and rewritten[closed_idx].startswith("CLOSED:")
    if status == "done":
        closed_line = f"CLOSED: [{timestamp}]"
        if has_closed:
            rewritten[closed_idx] = closed_line
        else:
            rewritten.insert(closed_idx, closed_line)
    elif has_closed:
        del rewritten[closed_idx]
    return rewritten


def org_timestamp() -> str:
    return datetime.now().strftime("%Y-%m-%d %a %H:%M")


def open_file_in_editor(rel: str, line: int) -> None:
    path = resolve_repo_file(rel)
    command = build_editor_command(path, max(1, line), os.environ.get("EDITOR", "emacsclient"))
    subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def resolve_repo_file(rel: str) -> Path:
    path_part = rel.split("::", 1)[0].split("#", 1)[0]
    path = (ROOT / path_part).resolve()
    if path != ROOT and ROOT not in path.parents:
        raise ValueError("file must stay inside repository")
    if not path.is_file():
        raise ValueError(f"file not found: {rel}")
    return path


def build_editor_command(path: Path, line: int, editor: str) -> list[str]:
    command = shlex.split(editor or "emacsclient")
    if not command:
        command = ["emacsclient"]
    executable = Path(command[0]).name
    if executable == "emacsclient" and "-n" not in command and "--no-wait" not in command:
        command.append("-n")
    command.extend([f"+{max(1, line)}", str(path)])
    return command


def open_browser(url: str) -> None:
    xdg_open = shutil.which("xdg-open")
    if xdg_open:
        subprocess.Popen([xdg_open, url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return
    webbrowser.open(url, new=2)


def main() -> int:
    if not (VISUALIZER / "index.html").exists():
        print("context visualizer files are missing", file=sys.stderr)
        return 1
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    url = f"http://127.0.0.1:{server.server_port}/context/visualizer/index.html"
    print(f"Serving MonadC context visualizer at {url}")
    open_browser(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nContext visualizer stopped.")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
