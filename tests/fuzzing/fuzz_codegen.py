#!/usr/bin/env python3
"""monadc typed/oracle fuzz runner.

This runner is intentionally self-contained: it discovers .fuzz property files
recursively, generates typed expressions for property arguments, compiles/runs
monadc programs, isolates failing laws, and keeps replay artifacts.

Property format, legacy compatible:

    name: string_contains_self
    section: strings
    args: s:String
    type: Bool
    expect: True
    description: Every generated string contains itself.
    law: (contains? {s} {s})

Optional oracle format:

    expect-python: needle in haystack

The oracle expression runs in a small sandbox over generated Python values.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import operator
import os
import random
import re
import shutil
import string
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

ROOT = Path(__file__).resolve().parents[2]
FUZZ_ROOT = Path(__file__).resolve().parent
PROPERTY_ROOT = FUZZ_ROOT / "properties"
MONAD = ROOT / "monad"
LAST_ROOT = FUZZ_ROOT / "last"
DEFAULT_WIDTH = 120

# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class Expr:
    typ: str
    src: str
    value: Any
    note: str = ""


@dataclasses.dataclass(frozen=True)
class PropertySpec:
    name: str
    section: str
    args: tuple[tuple[str, str], ...]
    typ: str
    expect: str | None
    expect_python: str | None
    description: str
    law: str
    path: Path
    line: int
    xfail: bool = False

    @property
    def relpath(self) -> str:
        try:
            return self.path.relative_to(ROOT).as_posix()
        except ValueError:
            return self.path.as_posix()

    @property
    def where(self) -> str:
        return f"{self.path.resolve()}:{self.line}:1"

    @property
    def arg_summary(self) -> str:
        return " ".join(f"{name}:{typ}" for name, typ in self.args) or "∅"

    @property
    def expectation_summary(self) -> str:
        if self.expect_python:
            return "python"
        return str(self.expect)


@dataclasses.dataclass(frozen=True)
class GeneratedLaw:
    spec: PropertySpec
    seed: int
    ordinal: int
    src: str
    src_before_rewrite: str
    expected: str
    bindings: Mapping[str, Expr]
    rewrites: tuple[str, ...]
    gen_elapsed_ns: int


@dataclasses.dataclass(frozen=True)
class ExecutionResult:
    ok: bool
    phase: str
    returncode: int
    compile_ns: int
    run_ns: int
    stdout: str
    stderr_or_output: str
    message: str


@dataclasses.dataclass(frozen=True)
class LawOutcome:
    law: GeneratedLaw
    ok: bool
    status: str
    phase: str
    actual: str | None
    expected: str
    elapsed_ns: int
    replay_path: Path | None
    message: str = ""
    compiler_output: str = ""


@dataclasses.dataclass
class RunStats:
    loaded: int = 0
    selected: int = 0
    malformed: int = 0
    generated: int = 0
    compiled_programs: int = 0
    executed_laws: int = 0
    passed: int = 0
    failed: int = 0
    blocked: int = 0
    xfailed: int = 0
    xpassed: int = 0
    rewrites: int = 0


@dataclasses.dataclass(frozen=True)
class Cli:
    seed: int
    cases: int
    laws_per_case: int
    depth: int
    properties: set[str]
    sections: set[str]
    keep: bool
    fail_fast: bool
    only_coverage: bool
    no_coverage: bool
    inventory: str
    show_expr: bool
    list_properties: bool
    json_summary: Path | None
    width: int
    max_failures: int


# ---------------------------------------------------------------------------
# Stable generated prelude
# ---------------------------------------------------------------------------

STABLE_DEFS = """\
define fuzz-id-int :: Int -> Int
  x -> x

define fuzz-add :: Int -> Int -> Int
  x y -> (+ x y)

define fuzz-sub :: Int -> Int -> Int
  x y -> (- x y)

define fuzz-mul :: Int -> Int -> Int
  x y -> (* x y)

define fuzz-lt? :: Int -> Int -> Bool
  x y -> (< x y)

define fuzz-lte? :: Int -> Int -> Bool
  x y -> (<= x y)

define fuzz-gt? :: Int -> Int -> Bool
  x y -> (> x y)

define fuzz-gte? :: Int -> Int -> Bool
  x y -> (>= x y)

define fuzz-eq-int? :: Int -> Int -> Bool
  x y -> (= x y)

define fuzz-choose-less :: Int -> Int -> Int -> Int
  x y fallback -> (if (< x y) x fallback)

define fuzz-min :: Int -> Int -> Int
  x y -> (if (< x y) x y)

define fuzz-max :: Int -> Int -> Int
  x y -> (if (> x y) x y)

define fuzz-bool-id? :: Bool -> Bool
  x -> x
"""

# ---------------------------------------------------------------------------
# Formatting: no ANSI by default; compiler-style paths only on failures/details.
# ---------------------------------------------------------------------------

def now() -> str:
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def hr(title: str, width: int = DEFAULT_WIDTH) -> None:
    title = f" {title} "
    line = "─" * max(0, width - len(title))
    left = line[: len(line) // 2]
    right = line[len(line) // 2 :]
    print(f"{left}{title}{right}")


def box(title: str, subtitle: str, width: int = DEFAULT_WIDTH) -> None:
    # Plain, stable text: no colors and no wide unicode frame.
    print()
    print(title)
    print("=" * len(title))
    if subtitle:
        print(subtitle)
    print()


def duration(ns: int) -> str:
    if ns < 1_000:
        return f"{ns:7.0f} ns"
    if ns < 1_000_000:
        return f"{ns / 1_000:7.2f} μs"
    if ns < 1_000_000_000:
        return f"{ns / 1_000_000:7.2f} ms"
    return f"{ns / 1_000_000_000:7.2f} s "


def truncate(s: str, width: int) -> str:
    if width <= 0:
        return ""
    if len(s) <= width:
        return s
    if width <= 1:
        return "…"
    return s[: width - 1] + "…"


def render_table(rows: Sequence[Mapping[str, Any]], columns: Sequence[tuple[str, str, int | None]], *, indent: str = "") -> None:
    if not rows:
        return
    widths: dict[str, int] = {}
    for key, heading, max_width in columns:
        raw = [str(row.get(key, "")) for row in rows]
        width = max([len(heading), *(len(v) for v in raw)])
        if max_width is not None:
            width = min(width, max_width)
        widths[key] = width

    head = "  ".join(heading.ljust(widths[key]) for key, heading, _ in columns)
    rule = "  ".join("─" * widths[key] for key, _, _ in columns)
    print(indent + head)
    print(indent + rule)
    for row in rows:
        parts: list[str] = []
        for key, _heading, max_width in columns:
            value = str(row.get(key, ""))
            if max_width is not None:
                value = truncate(value, widths[key])
            parts.append(value.ljust(widths[key]))
        print(indent + "  ".join(parts))


def compiler_line(path: Path, line: int, col: int, severity: str, message: str) -> str:
    return f"{path.resolve()}:{line}:{col}: {severity}: {message}"


# ---------------------------------------------------------------------------
# S-expression parsing for safe source normalization.
# ---------------------------------------------------------------------------

TokenTree = str | list["TokenTree"]


def tokenize_sexpr(src: str) -> list[str]:
    tokens: list[str] = []
    i = 0
    while i < len(src):
        ch = src[i]
        if ch.isspace():
            i += 1
            continue
        if ch in "()":
            tokens.append(ch)
            i += 1
            continue
        if ch == '"':
            j = i + 1
            escaped = False
            while j < len(src):
                c = src[j]
                if escaped:
                    escaped = False
                elif c == "\\":
                    escaped = True
                elif c == '"':
                    j += 1
                    break
                j += 1
            tokens.append(src[i:j])
            i = j
            continue
        j = i
        while j < len(src) and not src[j].isspace() and src[j] not in "()":
            j += 1
        tokens.append(src[i:j])
        i = j
    return tokens


def parse_sexpr_tokens(tokens: Sequence[str]) -> TokenTree:
    pos = 0

    def parse_one() -> TokenTree:
        nonlocal pos
        if pos >= len(tokens):
            raise ValueError("unexpected end of expression")
        tok = tokens[pos]
        pos += 1
        if tok == "(":
            items: list[TokenTree] = []
            while pos < len(tokens) and tokens[pos] != ")":
                items.append(parse_one())
            if pos >= len(tokens):
                raise ValueError("missing ')' in expression")
            pos += 1
            return items
        if tok == ")":
            raise ValueError("unexpected ')' in expression")
        return tok

    tree = parse_one()
    if pos != len(tokens):
        raise ValueError("trailing tokens after expression")
    return tree


def render_sexpr(tree: TokenTree) -> str:
    if isinstance(tree, str):
        return tree
    return "(" + " ".join(render_sexpr(item) for item in tree) + ")"


def rewrite_tree(tree: TokenTree, rewrites: list[str]) -> TokenTree:
    if isinstance(tree, str):
        return tree
    rewritten = [rewrite_tree(item, rewrites) for item in tree]
    if rewritten and rewritten[0] == "=" and len(rewritten) > 3:
        operands = rewritten[1:]
        expr: TokenTree = ["=", operands[-2], operands[-1]]
        for left, right in reversed(list(zip(operands[:-2], operands[1:-1]))):
            expr = ["if", ["=", left, right], expr, "False"]
        rewrites.append(f"lowered variadic '=' with {len(operands)} operands")
        return expr
    return rewritten


def normalize_law_source(src: str) -> tuple[str, tuple[str, ...]]:
    # Best effort only. If parsing fails, preserve source exactly.
    try:
        tree = parse_sexpr_tokens(tokenize_sexpr(src))
        rewrites: list[str] = []
        new_tree = rewrite_tree(tree, rewrites)
        return render_sexpr(new_tree), tuple(rewrites)
    except Exception:
        return src, ()


# ---------------------------------------------------------------------------
# Literal rendering and type-directed expression generation.
# ---------------------------------------------------------------------------

def mon_bool(v: bool) -> str:
    return "True" if bool(v) else "False"


def string_lit(value: str) -> str:
    escaped = (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\t", "\\t")
        .replace("\r", "\\r")
    )
    return f'"{escaped}"'


def char_lit(value: str) -> str:
    if value == "\\":
        return "'\\\\'"
    if value == "'":
        return "'\\\''"
    if value == "\n":
        return "'\\n'"
    if value == "\t":
        return "'\\t'"
    return f"'{value}'"


def path_lit(value: str) -> str:
    return value


def int_lit(rng: random.Random) -> Expr:
    # Bias toward interesting compiler/runtime edges, not just a flat range.
    edges = [-64, -17, -13, -12, -8, -3, -2, -1, 0, 1, 2, 3, 7, 8, 12, 13, 17, 31, 64]
    if rng.random() < 0.55:
        value = rng.choice(edges)
    else:
        value = rng.randint(-96, 96)
    return Expr("Int", str(value), value)


def nat_lit(rng: random.Random) -> Expr:
    value = rng.choice([0, 1, 2, 3, 4, 5, 8, 13]) if rng.random() < 0.6 else rng.randint(0, 24)
    return Expr("Nat", str(value), value)


def float_lit(rng: random.Random) -> Expr:
    value = rng.choice([-3.5, -1.0, -0.5, 0.0, 0.5, 1.0, 2.25, 10.0]) if rng.random() < 0.7 else rng.uniform(-20, 20)
    src = f"{value:.6g}"
    if "." not in src and "e" not in src.lower():
        src += ".0"
    return Expr("Float", src, float(value))


def gen_bool(rng: random.Random, depth: int) -> Expr:
    if depth <= 0 or rng.random() < 0.18:
        value = bool(rng.getrandbits(1))
        return Expr("Bool", mon_bool(value), value)

    choice = rng.choice(["not", "eq", "ne", "lt", "lte", "gt", "gte", "if", "do", "id"])
    if choice == "not":
        x = gen_bool(rng, depth - 1)
        return Expr("Bool", f"(not {x.src})", not bool(x.value))
    if choice == "id":
        x = gen_bool(rng, depth - 1)
        return Expr("Bool", f"(fuzz-bool-id? {x.src})", bool(x.value))
    if choice in {"eq", "ne", "lt", "lte", "gt", "gte"}:
        a = gen_int(rng, depth - 1)
        b = gen_int(rng, depth - 1)
        ops: dict[str, tuple[str, Any]] = {
            "eq": ("=", operator.eq),
            "ne": ("!=", operator.ne),
            "lt": ("<", operator.lt),
            "lte": ("<=", operator.le),
            "gt": (">", operator.gt),
            "gte": (">=", operator.ge),
        }
        op_src, op = ops[choice]
        return Expr("Bool", f"({op_src} {a.src} {b.src})", bool(op(int(a.value), int(b.value))))
    if choice == "if":
        c = gen_bool(rng, depth - 1)
        t = gen_bool(rng, depth - 1)
        e = gen_bool(rng, depth - 1)
        return Expr("Bool", f"(if {c.src} {t.src} {e.src})", bool(t.value if c.value else e.value))

    setup = gen_int(rng, depth - 1)
    body = gen_bool(rng, depth - 1)
    return Expr("Bool", f"(do {setup.src} {body.src})", bool(body.value))


def gen_int_with_var(rng: random.Random, depth: int, name: str, value: int) -> Expr:
    if depth <= 0 or rng.random() < 0.35:
        return Expr("Int", name, value)
    other = gen_int(rng, depth - 1)
    op_src, op_fn = rng.choice([
        ("fuzz-add", operator.add),
        ("fuzz-sub", operator.sub),
        ("fuzz-mul", operator.mul),
    ])
    if rng.getrandbits(1):
        return Expr("Int", f"({op_src} {name} {other.src})", op_fn(value, int(other.value)))
    return Expr("Int", f"({op_src} {other.src} {name})", op_fn(int(other.value), value))


def gen_int(rng: random.Random, depth: int) -> Expr:
    if depth <= 0:
        return int_lit(rng)

    choices = ["lit", "id", "add", "sub", "mul", "if", "with", "do", "min", "max"]
    choice = rng.choice(choices)
    if choice == "lit":
        return int_lit(rng)
    if choice == "id":
        x = gen_int(rng, depth - 1)
        return Expr("Int", f"(fuzz-id-int {x.src})", int(x.value))
    if choice in {"add", "sub", "mul"}:
        a = gen_int(rng, depth - 1)
        b = gen_int(rng, depth - 1)
        op_src = {"add": "fuzz-add", "sub": "fuzz-sub", "mul": "fuzz-mul"}[choice]
        op_fn = {"add": operator.add, "sub": operator.sub, "mul": operator.mul}[choice]
        return Expr("Int", f"({op_src} {a.src} {b.src})", int(op_fn(int(a.value), int(b.value))))
    if choice == "min":
        a = gen_int(rng, depth - 1)
        b = gen_int(rng, depth - 1)
        return Expr("Int", f"(fuzz-min {a.src} {b.src})", min(int(a.value), int(b.value)))
    if choice == "max":
        a = gen_int(rng, depth - 1)
        b = gen_int(rng, depth - 1)
        return Expr("Int", f"(fuzz-max {a.src} {b.src})", max(int(a.value), int(b.value)))
    if choice == "if":
        c = gen_bool(rng, depth - 1)
        t = gen_int(rng, depth - 1)
        e = gen_int(rng, depth - 1)
        return Expr("Int", f"(if {c.src} {t.src} {e.src})", int(t.value if c.value else e.value))
    if choice == "with":
        bound = gen_int(rng, depth - 1)
        body = gen_int_with_var(rng, depth - 1, "x", int(bound.value))
        return Expr("Int", f"(with [x {bound.src}] {body.src})", int(body.value))

    setup = gen_bool(rng, depth - 1) if rng.random() < 0.25 else gen_int(rng, depth - 1)
    body = gen_int(rng, depth - 1)
    return Expr("Int", f"(do {setup.src} {body.src})", int(body.value))


def gen_string(rng: random.Random, depth: int, *, non_empty: bool = False) -> Expr:
    atoms = ["", "a", "b", "monadc", "nad", "hello", "with space", "abcabc", "_x_", "123", "quote\\\"x"]
    if rng.random() < 0.45:
        value = rng.choice([s for s in atoms if s or not non_empty])
    else:
        alphabet = "abcxyz012 _-"
        lo = 1 if non_empty else 0
        value = "".join(rng.choice(alphabet) for _ in range(rng.randint(lo, 16)))
    return Expr("String", string_lit(value), value)


def gen_char(rng: random.Random, depth: int) -> Expr:
    value = rng.choice(list("abcxyz012 _-") + ["\n", "\t"])
    return Expr("Char", char_lit(value), value)


def gen_keyword(rng: random.Random, depth: int) -> Expr:
    value = rng.choice([":a", ":b", ":monadc", ":fuzz", ":x-y", ":_k"])
    return Expr("Keyword", value, value)


def gen_path(rng: random.Random, depth: int) -> Expr:
    value = rng.choice([
        "./context",
        "./tests/fuzzing",
        "../monadc",
        "/tmp/monadc-fuzz",
        "~/xos/projects/c/monadc/context",
    ])
    return Expr("Path", path_lit(value), value)


def arr_src(values: Sequence[int], *, heap: bool = False) -> str:
    prefix = "~" if heap else ""
    return prefix + "[" + " ".join(str(v) for v in values) + "]"


def gen_arr_int(rng: random.Random, depth: int, *, heap: bool = False, non_empty: bool = False) -> Expr:
    lo = 1 if non_empty else 0
    n = rng.randint(lo, 6)
    values = [int_lit(rng).value for _ in range(n)]
    typ = "HeapArrInt" if heap else "ArrInt"
    return Expr(typ, arr_src(values, heap=heap), tuple(int(v) for v in values))


def gen_list_int(rng: random.Random, depth: int, *, non_empty: bool = False) -> Expr:
    lo = 1 if non_empty else 0
    n = rng.randint(lo, 6)
    values = [int_lit(rng).value for _ in range(n)]
    if not values:
        return Expr("ListInt", "[]", tuple())
    # monadc list syntax in property packs may vary; keep the conservative literal form.
    return Expr("ListInt", "[" + " ".join(str(v) for v in values) + "]", tuple(int(v) for v in values))


def base_type_name(typ: str) -> str:
    return typ.strip()


def gen_dependent_arg(rng: random.Random, typ: str, depth: int, bindings: Mapping[str, Expr]) -> Expr | None:
    # Supported fine-grained type annotations in .fuzz args:
    #   IndexOf(xs), SubstrOf(s), ElemOf(xs), DifferentFrom(x)
    m = re.fullmatch(r"IndexOf\(([^)]+)\)", typ)
    if m:
        target = bindings.get(m.group(1))
        values = tuple(target.value) if target else tuple()
        idx = 0 if not values else rng.randrange(0, len(values))
        return Expr("Int", str(idx), idx, note=f"index of {m.group(1)}")

    m = re.fullmatch(r"SubstrOf\(([^)]+)\)", typ)
    if m:
        target = bindings.get(m.group(1))
        text = str(target.value) if target else ""
        if not text:
            sub = ""
        else:
            i = rng.randrange(0, len(text))
            j = rng.randrange(i, len(text) + 1)
            sub = text[i:j]
        return Expr("String", string_lit(sub), sub, note=f"substring of {m.group(1)}")

    m = re.fullmatch(r"ElemOf\(([^)]+)\)", typ)
    if m:
        target = bindings.get(m.group(1))
        values = tuple(target.value) if target else tuple()
        value = int(rng.choice(values)) if values else 0
        return Expr("Int", str(value), value, note=f"element of {m.group(1)}")

    m = re.fullmatch(r"DifferentFrom\(([^)]+)\)", typ)
    if m:
        target = bindings.get(m.group(1))
        if target and target.typ in {"Int", "Nat"}:
            value = int(target.value) + rng.choice([-3, -1, 1, 3])
            return Expr("Int", str(value), value, note=f"different from {m.group(1)}")
        if target and target.typ == "String":
            value = str(target.value) + "x"
            return Expr("String", string_lit(value), value, note=f"different from {m.group(1)}")
    return None


def gen_arg(rng: random.Random, typ: str, depth: int, bindings: Mapping[str, Expr]) -> Expr:
    typ = base_type_name(typ)
    dependent = gen_dependent_arg(rng, typ, depth, bindings)
    if dependent is not None:
        return dependent

    if typ in {"Int", "Integer"}:
        return gen_int(rng, depth)
    if typ == "Nat":
        return nat_lit(rng)
    if typ == "Bool":
        return gen_bool(rng, depth)
    if typ in {"Float", "Double"}:
        return float_lit(rng)
    if typ == "String":
        return gen_string(rng, depth)
    if typ == "NonEmptyString":
        return gen_string(rng, depth, non_empty=True)
    if typ == "Char":
        return gen_char(rng, depth)
    if typ == "Keyword":
        return gen_keyword(rng, depth)
    if typ == "Path":
        return gen_path(rng, depth)
    if typ in {"ArrInt", "ArrayInt", "Arr[Int]"}:
        return gen_arr_int(rng, depth)
    if typ in {"NonEmptyArrInt", "NonEmptyArrayInt"}:
        return gen_arr_int(rng, depth, non_empty=True)
    if typ in {"HeapArrInt", "HeapArrayInt"}:
        return gen_arr_int(rng, depth, heap=True)
    if typ in {"NonEmptyHeapArrInt", "NonEmptyHeapArrayInt"}:
        return gen_arr_int(rng, depth, heap=True, non_empty=True)
    if typ in {"ListInt", "List[Int]"}:
        return gen_list_int(rng, depth)
    if typ == "NonEmptyListInt":
        return gen_list_int(rng, depth, non_empty=True)
    raise ValueError(f"unsupported fuzz argument type {typ!r}")


# ---------------------------------------------------------------------------
# Property parsing and selection.
# ---------------------------------------------------------------------------

PLACEHOLDER_RE = re.compile(r"\{([A-Za-z_][A-Za-z0-9_]*)\}")


def parse_bool(value: str) -> bool:
    if value.lower() in {"true", "yes", "1"}:
        return True
    if value.lower() in {"false", "no", "0"}:
        return False
    raise ValueError(f"expected boolean, got {value!r}")


def load_property(path: Path) -> PropertySpec:
    data: dict[str, str] = {}
    line_numbers: dict[str, int] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if ":" not in line:
            raise ValueError(f"{path}:{line_number}: expected 'key: value'")
        key, value = line.split(":", 1)
        key = key.strip()
        data[key] = value.strip()
        line_numbers.setdefault(key, line_number)

    required = ["name", "section", "args", "type", "description", "law"]
    missing = [key for key in required if key not in data]
    if missing:
        raise ValueError(f"{path}: missing keys: {', '.join(missing)}")
    if "expect" not in data and "expect-python" not in data:
        raise ValueError(f"{path}: missing one of: expect, expect-python")

    args: list[tuple[str, str]] = []
    for item in data.get("args", "").split():
        if ":" not in item:
            raise ValueError(f"{path}:{line_numbers.get('args', 1)}: bad arg {item!r}; expected name:Type")
        name, typ = item.split(":", 1)
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise ValueError(f"{path}:{line_numbers.get('args', 1)}: bad arg name {name!r}")
        args.append((name, typ))

    declared = {name for name, _typ in args}
    used = set(PLACEHOLDER_RE.findall(data["law"]))
    undeclared = sorted(used - declared)
    if undeclared:
        raise ValueError(f"{path}:{line_numbers['law']}: undeclared placeholder(s): {', '.join(undeclared)}")

    return PropertySpec(
        name=data["name"],
        section=data["section"],
        args=tuple(args),
        typ=data["type"],
        expect=data.get("expect"),
        expect_python=data.get("expect-python"),
        description=data["description"],
        law=data["law"],
        path=path,
        line=line_numbers["law"],
        xfail=parse_bool(data.get("xfail", "False")),
    )


def load_properties() -> list[PropertySpec]:
    paths = sorted(
        (path for path in PROPERTY_ROOT.rglob("*.fuzz") if path.is_file()),
        key=lambda p: p.relative_to(PROPERTY_ROOT).as_posix(),
    )
    if not paths:
        raise SystemExit(f"no fuzz property files found recursively in {PROPERTY_ROOT}")

    specs: list[PropertySpec] = []
    seen: dict[str, Path] = {}
    errors: list[str] = []
    for path in paths:
        try:
            spec = load_property(path)
            if spec.name in seen:
                errors.append(
                    f"duplicate fuzz property name {spec.name!r}: "
                    f"{seen[spec.name].relative_to(ROOT).as_posix()} and {path.relative_to(ROOT).as_posix()}"
                )
                continue
            seen[spec.name] = path
            specs.append(spec)
        except Exception as exc:
            errors.append(str(exc))

    if errors:
        for err in errors:
            print(f"{Path(__file__).resolve()}:1:1: error: {err}", file=sys.stderr)
        raise SystemExit(2)
    return specs


def select_properties(specs: Sequence[PropertySpec], names: set[str], sections: set[str]) -> list[PropertySpec]:
    selected = list(specs)
    if names:
        by_name = {spec.name: spec for spec in specs}
        missing = sorted(names - set(by_name))
        if missing:
            raise SystemExit(f"unknown fuzz properties: {', '.join(missing)}")
        selected = [by_name[name] for name in sorted(names)]
    if sections:
        selected = [spec for spec in selected if spec.section in sections]
        if not selected:
            raise SystemExit(f"no properties selected for sections: {', '.join(sorted(sections))}")
    return selected


# ---------------------------------------------------------------------------
# Oracle and law generation.
# ---------------------------------------------------------------------------

SAFE_ORACLE_GLOBALS = {
    "__builtins__": {},
    "abs": abs,
    "all": all,
    "any": any,
    "bool": bool,
    "int": int,
    "len": len,
    "max": max,
    "min": min,
    "sum": sum,
    "sorted": sorted,
    "str": str,
    "math": math,
}


def expected_show(spec: PropertySpec, bindings: Mapping[str, Expr]) -> str:
    if spec.expect_python:
        local = {name: expr.value for name, expr in bindings.items()}
        value = eval(spec.expect_python, SAFE_ORACLE_GLOBALS, local)  # noqa: S307 - local test oracle by design.
        if isinstance(value, bool):
            return mon_bool(value)
        return str(value)
    assert spec.expect is not None
    return spec.expect


def generate_law(spec: PropertySpec, rng: random.Random, seed: int, ordinal: int, depth: int) -> GeneratedLaw:
    start = time.perf_counter_ns()
    bindings: dict[str, Expr] = {}
    for name, typ in spec.args:
        bindings[name] = gen_arg(rng, typ, depth, bindings)

    src = spec.law
    for name, expr in bindings.items():
        src = src.replace("{" + name + "}", expr.src)
    before = src
    src, rewrites = normalize_law_source(src)
    expected = expected_show(spec, bindings)
    elapsed = time.perf_counter_ns() - start
    return GeneratedLaw(spec, seed, ordinal, src, before, expected, bindings, rewrites, elapsed)


def coverage_laws(specs: Sequence[PropertySpec], seed: int, depth: int) -> list[GeneratedLaw]:
    laws: list[GeneratedLaw] = []
    for i, spec in enumerate(specs, 1):
        rng = random.Random((seed * 1_000_003) + i)
        laws.append(generate_law(spec, rng, seed, i, depth))
    return laws


def random_batch(seed: int, laws_per_case: int, depth: int, specs: Sequence[PropertySpec]) -> list[GeneratedLaw]:
    rng = random.Random(seed)
    return [generate_law(rng.choice(specs), rng, seed, i, depth) for i in range(1, laws_per_case + 1)]


# ---------------------------------------------------------------------------
# Program construction and execution.
# ---------------------------------------------------------------------------

def build_program(seed: int, laws: Sequence[GeneratedLaw]) -> tuple[str, str]:
    lines = [
        ";; generated by tests/fuzzing/fuzz_codegen.py",
        f";; seed: {seed}",
        "(module Main)",
        "",
        STABLE_DEFS.rstrip(),
        "",
    ]
    expected: list[str] = []
    for index, law in enumerate(laws, 1):
        lines.append(f";; law {index}: {law.spec.name} from {law.spec.relpath}:{law.spec.line}")
        if law.rewrites:
            for rewrite in law.rewrites:
                lines.append(f";; rewrite: {rewrite}")
        lines.append(f"(show {law.src})")
        expected.append(law.expected)
    return "\n".join(lines) + "\n", "\n".join(expected) + "\n"


def ensure_last_root() -> Path:
    LAST_ROOT.mkdir(parents=True, exist_ok=True)
    return LAST_ROOT


def save_replay(label: str, seed: int, source: str, expected: str, actual: str, compiler_output: str = "") -> Path:
    out = ensure_last_root()
    safe_label = re.sub(r"[^A-Za-z0-9_.-]+", "_", label).strip("_")[:80] or "fuzz"
    stem = f"{safe_label}_seed_{seed:06d}"
    src = out / f"{stem}.mon"
    src.write_text(source, encoding="utf-8")
    (out / f"{stem}.expected").write_text(expected, encoding="utf-8")
    (out / f"{stem}.actual").write_text(actual, encoding="utf-8")
    if compiler_output:
        (out / f"{stem}.compiler").write_text(compiler_output, encoding="utf-8")
    return src


def compile_and_run(seed: int, laws: Sequence[GeneratedLaw], *, keep: bool, label: str) -> tuple[ExecutionResult, str, str, Path | None]:
    source, expected = build_program(seed, laws)
    with tempfile.TemporaryDirectory(prefix=f"monadc-fuzz-{label}-{seed}-") as tmp_name:
        tmp = Path(tmp_name)
        src = tmp / f"fuzz_{seed}.mon"
        exe = tmp / f"fuzz_{seed}"
        src.write_text(source, encoding="utf-8")

        compile_start = time.perf_counter_ns()
        compile_result = subprocess.run(
            [str(MONAD), str(src), "-o", str(exe)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        compile_ns = time.perf_counter_ns() - compile_start
        if compile_result.returncode != 0:
            replay = save_replay(label, seed, source, expected, "", compile_result.stdout)
            return (
                ExecutionResult(False, "compile", compile_result.returncode, compile_ns, 0, "", compile_result.stdout, "compile failed"),
                source,
                expected,
                replay,
            )

        run_start = time.perf_counter_ns()
        run_result = subprocess.run(
            [str(exe)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        run_ns = time.perf_counter_ns() - run_start
        ok = run_result.returncode == 0 and run_result.stdout == expected
        message = "ok"
        if run_result.returncode != 0:
            message = f"executable failed with exit code {run_result.returncode}"
        elif run_result.stdout != expected:
            message = "stdout mismatch"
        replay = None
        if keep or not ok:
            replay = save_replay(label, seed, source, expected, run_result.stdout)
        return (
            ExecutionResult(ok, "run", run_result.returncode, compile_ns, run_ns, run_result.stdout, run_result.stdout, message),
            source,
            expected,
            replay,
        )


def run_isolated_law(law: GeneratedLaw, *, keep: bool, label: str) -> LawOutcome:
    result, source, expected, replay = compile_and_run(law.seed, [law], keep=keep, label=f"{label}_{law.spec.name}")
    actual = result.stdout.splitlines()[0] if result.stdout.splitlines() else None
    ok = result.ok and actual == law.expected
    if law.spec.xfail:
        status = "XFAIL" if not ok else "XPASS"
    else:
        status = "PASS" if ok else "FAIL"
    elapsed = result.compile_ns + result.run_ns
    return LawOutcome(
        law=law,
        ok=ok,
        status=status,
        phase=result.phase,
        actual=actual,
        expected=law.expected,
        elapsed_ns=elapsed,
        replay_path=replay,
        message=result.message,
        compiler_output=result.stderr_or_output if not ok else "",
    )


def isolate_batch_failures(laws: Sequence[GeneratedLaw], *, keep: bool, label: str) -> list[LawOutcome]:
    return [run_isolated_law(law, keep=keep, label=label) for law in laws]


# ---------------------------------------------------------------------------
# Output rendering.
# ---------------------------------------------------------------------------

# Design rules:
#   * Every status row starts at column 1 in Emacs compilation-mode form:
#       /path/file:line:col: note:   [0001/0482] property_name PASS [  12.34 ] ms
#   * No ANSI colors and no repeated table headers.
#   * Passing rows are compact and aligned; failures get a clickable error row
#     followed by indented context and raw compiler diagnostics.

ROW_NAME_WIDTH = 48
ROW_COUNTER_WIDTH = 9
ROW_STATUS_WIDTH = 5
ROW_DETAIL_LIMIT = 96


def configure_row_widths(specs: Sequence[PropertySpec]) -> None:
    global ROW_NAME_WIDTH
    if not specs:
        ROW_NAME_WIDTH = 48
        return
    longest = max(len(spec.name) for spec in specs)
    ROW_NAME_WIDTH = max(44, min(longest, 72))


def emacs_prefix(path: Path, line: int, col: int, severity: str) -> str:
    return f"{path.resolve()}:{line}:{col}: {severity}:"


def bracket_duration(ns: int) -> str:
    if ns < 1_000:
        return f"[{ns:7.2f} ] ns"
    if ns < 1_000_000:
        return f"[{ns / 1_000:7.2f} ] μs"
    if ns < 1_000_000_000:
        return f"[{ns / 1_000_000:7.2f} ] ms"
    return f"[{ns / 1_000_000_000:7.2f} ] s"


def detail_suffix(detail: str) -> str:
    if not detail:
        return ""
    return " :: " + truncate(detail, ROW_DETAIL_LIMIT)


def print_status_line(
    *,
    path: Path,
    line: int,
    col: int = 1,
    severity: str = "note",
    counter: str,
    name: str,
    status: str,
    elapsed_ns: int,
    detail: str = "",
) -> None:
    # Starts at column 1 by construction.
    prefix = emacs_prefix(path, line, col, severity)
    printable_name = truncate(name, ROW_NAME_WIDTH)
    print(
        f"{prefix}   [{counter:<{ROW_COUNTER_WIDTH}}] "
        f"{printable_name:<{ROW_NAME_WIDTH}}  "
        f"{status:<{ROW_STATUS_WIDTH}} {bracket_duration(elapsed_ns)}"
        f"{detail_suffix(detail)}"
    )


def print_section(title: str) -> None:
    print()
    print(title)
    print("-" * len(title))


def print_config(cli: Cli, specs: Sequence[PropertySpec], selected: Sequence[PropertySpec]) -> None:
    sections = Counter(spec.section for spec in selected)
    arg_types = Counter(typ for spec in selected for _name, typ in spec.args)
    rewritable = sum(1 for spec in selected if re.search(r"\(\s*=\s+[^()]+\s+[^()]+\s+[^()]+", spec.law))
    nested = sum(1 for spec in selected if spec.path.relative_to(PROPERTY_ROOT).as_posix().count("/") > 0)

    print("run")
    rows = [
        ("root", ROOT.as_posix()),
        ("properties", f"{len(selected)}/{len(specs)} selected from {PROPERTY_ROOT.relative_to(ROOT).as_posix()}"),
        ("recursive", f"yes; nested={nested}"),
        ("seed", str(cli.seed)),
        ("cases", str(cli.cases)),
        ("laws/case", str(cli.laws_per_case)),
        ("depth", str(cli.depth)),
        ("coverage", "off" if cli.no_coverage else "on"),
        ("fail-fast", str(cli.fail_fast)),
        ("rewritable '='", str(rewritable)),
    ]
    keyw = max(len(k) for k, _v in rows)
    for key, value in rows:
        print(f"  {key:<{keyw}}  {value}")

    print()
    print("profile")
    # Keep this readable: show the most common sections first, not one massive line.
    common_sections = sections.most_common(12)
    shown = ", ".join(f"{k}={v}" for k, v in common_sections) or "none"
    remainder = len(sections) - len(common_sections)
    if remainder > 0:
        shown += f", … +{remainder} sections"
    args = ", ".join(f"{k}={v}" for k, v in sorted(arg_types.items())) or "none"
    print(f"  sections   {shown}")
    print(f"  arg types  {args}")


def inventory_rows(specs: Sequence[PropertySpec]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    total = len(specs)
    for i, spec in enumerate(specs, 1):
        rows.append({
            "#": f"{i:04d}/{total:04d}",
            "state": "READY",
            "section": spec.section,
            "name": spec.name,
            "args": spec.arg_summary,
            "expect": spec.expectation_summary,
            "file": spec.relpath,
        })
    return rows


def print_inventory(specs: Sequence[PropertySpec], mode: str) -> None:
    if mode == "none":
        return
    print_section("inventory")
    nested_specs = [spec for spec in specs if spec.path.relative_to(PROPERTY_ROOT).as_posix().count("/") > 0]
    print(f"  loaded      {len(specs)} properties")
    print("  recursive   yes")
    print(f"  nested      {'yes' if nested_specs else 'no'}" + (f" ({len(nested_specs)} properties)" if nested_specs else ""))
    if mode != "table":
        print("  listing     compact; use --inventory table for every READY row")
        if nested_specs:
            print("  examples")
            total = len(specs)
            for spec in nested_specs[:5]:
                idx = specs.index(spec) + 1
                print_status_line(
                    path=spec.path,
                    line=spec.line,
                    severity="note",
                    counter=f"{idx:04d}/{total:04d}",
                    name=spec.name,
                    status="READY",
                    elapsed_ns=0,
                    detail=f"{spec.section} {spec.arg_summary} -> {spec.typ} | expect:{spec.expectation_summary}",
                )
        return

    total = len(specs)
    for i, spec in enumerate(specs, 1):
        print_status_line(
            path=spec.path,
            line=spec.line,
            severity="note",
            counter=f"{i:04d}/{total:04d}",
            name=spec.name,
            status="READY",
            elapsed_ns=0,
            detail=f"{spec.section} {spec.arg_summary} -> {spec.typ} | expect:{spec.expectation_summary}",
        )


def print_compiler_output(output: str, limit: int = 80) -> None:
    if not output.strip():
        return
    lines = output.rstrip().splitlines()[-limit:]
    for line in lines:
        # Preserve compiler/emacs-clickable diagnostics at column 1.
        if re.match(r"^(?:/|[A-Za-z]:\\|[^\s:]+\.mon:)", line):
            print(line)
        else:
            print(f"  | {line}")


def outcome_detail(outcome: LawOutcome) -> str:
    law = outcome.law
    parts = [
        f"seed={law.seed:06d}",
        f"phase={outcome.phase}",
        f"section={law.spec.section}",
        f"expect={outcome.expected}",
        f"actual={outcome.actual if outcome.actual is not None else '∅'}",
    ]
    if law.rewrites:
        parts.append(f"rewrites={len(law.rewrites)}")
    return " ".join(parts)


def print_diagnostic(outcome: LawOutcome, *, show_expr: bool = True) -> None:
    law = outcome.law
    if outcome.replay_path:
        try:
            replay = outcome.replay_path.relative_to(ROOT).as_posix()
        except ValueError:
            replay = outcome.replay_path.as_posix()
        print(f"  replay    {replay}")
    print(f"  property  {law.spec.name}")
    print(f"  section   {law.spec.section}")
    print(f"  seed      {law.seed}")
    print(f"  phase     {outcome.phase}")
    print(f"  message   {outcome.message}")
    print(f"  expect    {outcome.expected}")
    print(f"  actual    {outcome.actual if outcome.actual is not None else '∅'}")
    if show_expr:
        print(f"  expr      {law.src}")
        if law.src != law.src_before_rewrite:
            print(f"  before    {law.src_before_rewrite}")
        if law.rewrites:
            print(f"  rewrite   {'; '.join(law.rewrites)}")
        if law.bindings:
            bindings = ", ".join(f"{k}={v.src}" for k, v in law.bindings.items())
            print(f"  bindings  {bindings}")
    print_compiler_output(outcome.compiler_output)


def print_law_outcome(index: str, outcome: LawOutcome, *, show_expr: bool = False) -> None:
    severity = "note" if outcome.ok else "error"
    message = outcome_detail(outcome) if outcome.ok else (outcome.message or outcome.status)
    print_status_line(
        path=outcome.law.spec.path,
        line=outcome.law.spec.line,
        severity=severity,
        counter=index,
        name=outcome.law.spec.name,
        status=outcome.status,
        elapsed_ns=outcome.elapsed_ns,
        detail=message,
    )
    if not outcome.ok:
        print_diagnostic(outcome, show_expr=show_expr)
    elif show_expr:
        law = outcome.law
        print(f"  expr      {law.src}")
        if law.bindings:
            bindings = ", ".join(f"{k}={v.src}" for k, v in law.bindings.items())
            print(f"  bindings  {bindings}")


def print_batch_header() -> None:
    # Deliberately no header: random-batch rows follow the same compiler-mode convention.
    pass


def print_batch_row(seed: int, batch_index: int, total_batches: int, laws: Sequence[GeneratedLaw], result: ExecutionResult) -> None:
    severity = "note" if result.ok else "error"
    status = "PASS" if result.ok else "FAIL"
    elapsed = result.compile_ns + result.run_ns
    detail = (
        f"seed={seed:06d} phase={result.phase} laws={len(laws)} "
        f"compile={duration(result.compile_ns).strip()} run={duration(result.run_ns).strip()} {result.message}"
    )
    print_status_line(
        path=Path(__file__).resolve(),
        line=1,
        severity=severity,
        counter=f"{batch_index:04d}/{total_batches:04d}",
        name=f"fuzz.batch.{seed:06d}",
        status=status,
        elapsed_ns=elapsed,
        detail=detail,
    )


def print_failures(failures: Sequence[LawOutcome], max_failures: int) -> None:
    if not failures:
        return
    print_section("failures")
    print(f"  isolated failures: {len(failures)}")
    for i, failure in enumerate(failures[:max_failures], 1):
        print_law_outcome(f"{i:04d}/{len(failures):04d}", failure, show_expr=True)
        if i != min(len(failures), max_failures):
            print()
    if len(failures) > max_failures:
        print(f"... {len(failures) - max_failures} more failure(s) suppressed; raise --max-failures")


def print_summary(stats: RunStats, failures: Sequence[LawOutcome], started_ns: int) -> None:
    print_section("summary")
    rows = [
        ("properties", stats.selected),
        ("laws generated", stats.generated),
        ("laws executed", stats.executed_laws),
        ("passed", stats.passed),
        ("failed", stats.failed),
        ("blocked", stats.blocked),
        ("xfail", stats.xfailed),
        ("xpass", stats.xpassed),
        ("source rewrites", stats.rewrites),
        ("compiled programs", stats.compiled_programs),
        ("elapsed", duration(time.perf_counter_ns() - started_ns).strip()),
        ("result", f"FAIL ({len(failures)} isolated failure(s))" if failures else "PASS"),
    ]
    keyw = max(len(k) for k, _v in rows)
    for key, value in rows:
        print(f"  {key:<{keyw}}  {value}")


# ---------------------------------------------------------------------------
# Main run phases.
# ---------------------------------------------------------------------------

def run_coverage(specs: Sequence[PropertySpec], cli: Cli, stats: RunStats, failures: list[LawOutcome]) -> None:
    print_section("coverage")
    print(f"  one generated instance per property ({len(specs)} total)")

    laws = coverage_laws(specs, cli.seed, cli.depth)
    total = len(laws)
    for i, law in enumerate(laws, 1):
        stats.generated += 1
        stats.rewrites += len(law.rewrites)
        outcome = run_isolated_law(law, keep=cli.keep, label="coverage")
        stats.compiled_programs += 1
        stats.executed_laws += 1 if outcome.phase == "run" else 0
        if outcome.status == "PASS":
            stats.passed += 1
        elif outcome.status == "XFAIL":
            stats.xfailed += 1
        elif outcome.status == "XPASS":
            stats.xpassed += 1
            failures.append(outcome)
        else:
            stats.failed += 1
            failures.append(outcome)

        # Compact success stream; every row still follows the project convention.
        should_print = outcome.status != "PASS" or cli.show_expr or i == 1 or i == total or i % 25 == 0
        if should_print:
            print_law_outcome(f"{i:04d}/{total:04d}", outcome, show_expr=cli.show_expr or outcome.status != "PASS")
        if failures and cli.fail_fast:
            break


def run_random_batches(specs: Sequence[PropertySpec], cli: Cli, stats: RunStats, failures: list[LawOutcome]) -> None:
    if cli.only_coverage:
        return
    print_section("random batches")
    print(f"  {cli.cases} batches, {cli.laws_per_case} laws per batch")
    print_batch_header()

    for batch_index in range(1, cli.cases + 1):
        seed = cli.seed + batch_index - 1
        laws = random_batch(seed, cli.laws_per_case, cli.depth, specs)
        stats.generated += len(laws)
        stats.rewrites += sum(len(law.rewrites) for law in laws)
        result, _source, _expected, _replay = compile_and_run(seed, laws, keep=cli.keep, label=f"batch_{batch_index:04d}")
        stats.compiled_programs += 1
        should_print = (not result.ok) or batch_index == 1 or batch_index == cli.cases or batch_index % 10 == 0
        if should_print:
            print_batch_row(seed, batch_index, cli.cases, laws, result)
        if result.ok:
            stats.executed_laws += len(laws)
            stats.passed += len(laws)
            continue

        stats.blocked += len(laws)
        print(f"  batch {batch_index:04d} failed in {result.phase}; isolating {len(laws)} law(s)")
        isolated = isolate_batch_failures(laws, keep=True, label=f"batch_{batch_index:04d}_isolated")
        for j, outcome in enumerate(isolated, 1):
            stats.compiled_programs += 1
            if outcome.ok:
                continue
            if outcome.status == "XFAIL":
                stats.xfailed += 1
            else:
                stats.failed += 1
                failures.append(outcome)
            print_law_outcome(f"{j:04d}/{len(isolated):04d}", outcome, show_expr=True)
        if failures and cli.fail_fast:
            break

# ---------------------------------------------------------------------------
# CLI.
# ---------------------------------------------------------------------------

def parse_args() -> Cli:
    parser = argparse.ArgumentParser(description="monadc recursive typed/oracle fuzz runner")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--cases", type=int, default=96)
    parser.add_argument("--laws-per-case", "--forms", dest="laws_per_case", type=int, default=10)
    parser.add_argument("--depth", type=int, default=5)
    parser.add_argument("--properties", default="", help="comma-separated property names")
    parser.add_argument("--sections", default="", help="comma-separated section names")
    parser.add_argument("--keep", action="store_true", help="keep replay files for passing programs too")
    parser.add_argument("--fail-fast", action="store_true", help="stop after first isolated failure")
    parser.add_argument("--only-coverage", action="store_true", help="run one isolated instance per property and stop")
    parser.add_argument("--no-coverage", action="store_true", help="skip isolated property coverage phase")
    parser.add_argument(
        "--inventory",
        choices=["none", "summary", "table"],
        default="summary",
        help="inventory rendering; table is fully aligned but verbose",
    )
    parser.add_argument("--list-properties", action="store_true", help="same as --inventory table --only-coverage with no execution")
    parser.add_argument("--show-expr", action="store_true", help="show generated expressions and bindings")
    parser.add_argument("--json-summary", type=Path)
    parser.add_argument("--width", type=int, default=120)
    parser.add_argument("--max-failures", type=int, default=25)
    # Compatibility with older scripts / Makefiles. No color is emitted by this runner.
    parser.add_argument("--no-rich", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.cases < 0:
        raise SystemExit("--cases must be non-negative")
    if args.laws_per_case < 1:
        raise SystemExit("--laws-per-case must be positive")
    if args.depth < 0:
        raise SystemExit("--depth must be non-negative")
    if args.only_coverage and args.no_coverage:
        raise SystemExit("--only-coverage and --no-coverage conflict")

    properties = {item.strip() for item in args.properties.split(",") if item.strip()}
    sections = {item.strip() for item in args.sections.split(",") if item.strip()}
    inventory = "table" if args.list_properties else args.inventory
    return Cli(
        seed=args.seed,
        cases=args.cases,
        laws_per_case=args.laws_per_case,
        depth=args.depth,
        properties=properties,
        sections=sections,
        keep=args.keep,
        fail_fast=args.fail_fast,
        only_coverage=args.only_coverage,
        no_coverage=args.no_coverage,
        inventory=inventory,
        show_expr=args.show_expr,
        list_properties=args.list_properties,
        json_summary=args.json_summary,
        width=max(80, args.width),
        max_failures=args.max_failures,
    )


def write_json_summary(path: Path, stats: RunStats, failures: Sequence[LawOutcome], started_ns: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "stats": dataclasses.asdict(stats),
        "elapsed_ns": time.perf_counter_ns() - started_ns,
        "failures": [
            {
                "name": f.law.spec.name,
                "section": f.law.spec.section,
                "path": f.law.spec.relpath,
                "line": f.law.spec.line,
                "seed": f.law.seed,
                "status": f.status,
                "phase": f.phase,
                "expected": f.expected,
                "actual": f.actual,
                "message": f.message,
                "replay": f.replay_path.as_posix() if f.replay_path else None,
                "expr": f.law.src,
                "expr_before_rewrite": f.law.src_before_rewrite,
                "rewrites": list(f.law.rewrites),
            }
            for f in failures
        ],
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")


def main() -> int:
    cli = parse_args()
    specs = load_properties()
    selected = select_properties(specs, cli.properties, cli.sections)
    if not MONAD.exists() and not cli.list_properties:
        raise SystemExit(f"compiler not found at {MONAD}; run make first")

    configure_row_widths(selected)
    stats = RunStats(loaded=len(specs), selected=len(selected))
    subtitle = f"seed={cli.seed} depth={cli.depth} selected={len(selected)}/{len(specs)} recursive"
    box("monadc fuzz", subtitle, cli.width)
    print_config(cli, specs, selected)
    print_inventory(selected, cli.inventory)

    if cli.list_properties:
        return 0

    if LAST_ROOT.exists():
        # Keep directory but clear stale fuzz replay files; leave unknown user files alone.
        for child in LAST_ROOT.glob("*_seed_*.mon"):
            stem = child.with_suffix("")
            for suffix in [".mon", ".expected", ".actual", ".compiler"]:
                target = stem.with_suffix(suffix)
                if target.exists():
                    target.unlink()

    started_ns = time.perf_counter_ns()
    failures: list[LawOutcome] = []

    if not cli.no_coverage:
        run_coverage(selected, cli, stats, failures)

    if not (failures and cli.fail_fast):
        run_random_batches(selected, cli, stats, failures)

    print_failures(failures, cli.max_failures)
    print_summary(stats, failures, started_ns)
    if cli.json_summary:
        write_json_summary(cli.json_summary, stats, failures, started_ns)
        print(f"json summary: {cli.json_summary}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
