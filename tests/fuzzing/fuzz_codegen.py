#!/usr/bin/env python3
"""monadc compiler fuzz runner.

This file is deliberately self-contained.  It keeps the old `.fuzz` format
working, but adds the pieces the corpus now needs:

* recursive property discovery
* stable/experimental/oracle/compile-fail/stress tiers
* law, program, and compile-fail property kinds
* typed and strategy-annotated generators, e.g. `x:Int@edge+if`
* Python oracle support via `expect-python:`
* one-property coverage plus random mixed batches
* batch failure isolation
* simple shrink-on-failure
* feature/section coverage reporting
* failure clustering
* replay artifacts under tests/fuzzing/last
* output formatted like tests/run.py so Emacs compilation-mode can buttonize it

Legacy property format remains valid:

    name: int_add_commutative
    section: arithmetic
    args: a:Int b:Int
    type: Bool
    expect: True
    description: Addition commutes.
    law: (= (+ {a} {b}) (+ {b} {a}))

New optional fields:

    tier: stable | experimental | oracle | compile-fail | stress
    kind: law | program | compile-fail
    features: if,with,set!,array-index
    expect-python: Python oracle expression over generated values
    expect-diagnostic: substring expected in compiler output for compile-fail
    program: full program body, used by kind: program or kind: compile-fail
    xfail: True
"""

from __future__ import annotations

import argparse
import dataclasses
import difflib
import json
import math
import operator
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

ROOT = Path(__file__).resolve().parents[2]
TEST_ROOT = ROOT / "tests"
FUZZ_ROOT = Path(__file__).resolve().parent
PROPERTY_ROOT = FUZZ_ROOT / "properties"
MONAD = ROOT / "monad"
LAST_ROOT = FUZZ_ROOT / "last"
RESULTS_FILE = FUZZ_ROOT / ".fuzz-results.json"
FIRST_FAILURE_FILE = FUZZ_ROOT / ".last-first-failure.org"
WIDTH = 118

RESET      = "\033[0m"
WHITE      = "\033[97m"
BOLD_GREEN = "\033[1;32m"
BOLD_RED   = "\033[1;31m"
BOLD       = "\033[1m"
GREEN      = "\033[32m"
YELLOW     = "\033[33m"
GRAY       = "\033[90m"
CYAN       = "\033[36m"
BLUE       = "\033[34m"
MAGENTA    = "\033[35m"
BLACK_BOLD = "\033[1;30m"
BRACKET_COLOR = "\033[38;2;157;129;186m"

SECTION_STYLES = {
    "core": (BLUE, "Core Arithmetic and Order"),
    "control": (GREEN, "Control Flow and PHI"),
    "binding": (CYAN, "Bindings, Env, Mutation"),
    "dispatch": (MAGENTA, "Codegen Dispatch"),
    "runtime": (YELLOW, "Runtime Collections"),
    "quote": (BLUE, "Quote, Null, Path"),
    "cross": (MAGENTA, "Cross Feature Compiler Paths"),
    "negative": (YELLOW, "Negative and Invariant Probes"),
    "oracle": (CYAN, "Oracle Properties"),
    "stress": (MAGENTA, "Stress Programs"),
    "batch": (GREEN, "Random Mixed Batches"),
    "misc": (YELLOW, "Miscellaneous Fuzz Properties"),
}

SECTION_ORDER = {
    "core": 0,
    "control": 10,
    "binding": 20,
    "dispatch": 30,
    "runtime": 40,
    "quote": 50,
    "cross": 60,
    "negative": 90,
    "oracle": 95,
    "stress": 96,
    "batch": 100,
    "misc": 1000,
}

VALID_TIERS = {"stable", "experimental", "oracle", "compile-fail", "stress"}
VALID_KINDS = {"law", "program", "compile-fail"}


### Data model

@dataclass(frozen=True)
class Expr:
    typ: str
    src: str
    value: Any
    note: str = ""


@dataclass(frozen=True)
class ArgSpec:
    name: str
    typ: str
    strategies: tuple[str, ...] = ()

    @property
    def raw(self) -> str:
        suffix = "" if not self.strategies else "@" + "+".join(self.strategies)
        return f"{self.name}:{self.typ}{suffix}"


@dataclass(frozen=True)
class PropertySpec:
    name: str
    section: str
    tier: str
    kind: str
    args: tuple[ArgSpec, ...]
    typ: str
    expect: str | None
    expect_python: str | None
    expect_diagnostic: str | None
    description: str
    law: str | None
    program: str | None
    features: tuple[str, ...]
    path: Path
    line: int
    xfail: bool = False
    disabled: bool = False

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
        return " ".join(arg.raw for arg in self.args) or "∅"

    @property
    def expectation_summary(self) -> str:
        if self.expect_python:
            return "python"
        if self.expect_diagnostic:
            return "diagnostic"
        return str(self.expect)


@dataclass(frozen=True)
class GeneratedCase:
    spec: PropertySpec
    seed: int
    ordinal: int
    source: str
    source_before_rewrite: str
    expected: str
    bindings: Mapping[str, Expr]
    rewrites: tuple[str, ...]
    gen_elapsed_ns: int


@dataclass(frozen=True)
class ExecutionResult:
    ok: bool
    phase: str
    returncode: int
    compile_ns: int
    run_ns: int
    stdout: str
    output: str
    message: str


@dataclass(frozen=True)
class CaseOutcome:
    generated: GeneratedCase
    ok: bool
    status: str
    phase: str
    actual: str | None
    expected: str
    elapsed_ns: int
    replay_path: Path | None
    message: str = ""
    compiler_output: str = ""
    shrunk: GeneratedCase | None = None


@dataclass
class SectionStats:
    total: int = 0
    passed: int = 0
    failed: int = 0
    xfailed: int = 0
    xpassed: int = 0
    elapsed_ns: int = 0

    @property
    def percent(self) -> float:
        attempted = self.passed + self.failed + self.xfailed + self.xpassed
        return 100.0 if attempted == 0 else (self.passed + self.xfailed) / attempted * 100.0


@dataclass
class Widths:
    location: int = 0
    progress: int = 0


@dataclass
class RunStats:
    loaded: int = 0
    selected: int = 0
    disabled: int = 0
    malformed: int = 0
    generated: int = 0
    compiled_programs: int = 0
    executed_cases: int = 0
    passed: int = 0
    failed: int = 0
    blocked: int = 0
    xfailed: int = 0
    xpassed: int = 0
    rewrites: int = 0
    shrinks: int = 0


@dataclass(frozen=True)
class Cli:
    seed: int
    cases: int
    laws_per_case: int
    depth: int
    properties: set[str]
    sections: set[str]
    tiers: set[str]
    include_disabled: bool
    keep: bool
    fail_fast: bool
    only_coverage: bool
    no_coverage: bool
    no_batches: bool
    inventory: str
    show_expr: bool
    list_properties: bool
    json_summary: Path | None
    width: int
    max_failures: int
    shrink: bool
    max_shrink_steps: int
    feature_summary: bool


### Stable generated prelude

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


### Output copied in spirit from tests/run.py

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def strip_ansi(value: str) -> str:
    return ANSI_RE.sub("", value)


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name)


def background_code(color: str) -> str:
    match = re.search(r"\[(?:1;)?3(\d)m", color)
    digit = match.group(1) if match else "7"
    return f"\033[4{digit}m"


def print_section_line(title: str, color: str, *, bold: bool = False) -> None:
    bg = background_code(color)
    label = f" {title} "
    cap_right = f"{color}{chr(0x25d7)}{RESET}"
    print(f"{bg}{BLACK_BOLD}{label}{RESET}{cap_right}")


def print_box(title: str) -> None:
    print(f"╔{'═' * WIDTH}╗")
    print(f"║{title.center(WIDTH)}║")
    print(f"╚{'═' * WIDTH}╝")


def print_result_box(passed: bool) -> None:
    color = BOLD_GREEN if passed else BOLD_RED
    label = "FUZZ PASSED" if passed else "FUZZ FAILED"
    print(f"{color}╔{'═' * WIDTH}╗{RESET}")
    print(f"{color}║{label.center(WIDTH)}║{RESET}")
    print(f"{color}╚{'═' * WIDTH}╝{RESET}")


def print_rule(title: str) -> None:
    print_section_line(title, WHITE, bold=True)


def print_section_header(section: str) -> None:
    color, title = SECTION_STYLES.get(section, (YELLOW, section.title()))
    print()
    print_section_line(f"{title} ({section})", color, bold=True)


def format_duration(elapsed_ns: int) -> str:
    if elapsed_ns < 1_000:
        return color_duration(float(elapsed_ns), "ns")
    if elapsed_ns < 1_000_000:
        return color_duration(elapsed_ns / 1_000, "μs")
    if elapsed_ns < 1_000_000_000:
        return color_duration(elapsed_ns / 1_000_000, "ms")
    return color_duration(elapsed_ns / 1_000_000_000, "s")


def color_duration(value: float, unit: str) -> str:
    return f"{BRACKET_COLOR}[{RESET}{value:7.2f} {BRACKET_COLOR}]{RESET} {CYAN}{unit:>2}{RESET}"


def progress_prefix(
    loc_path: str,
    loc_line: int,
    loc_col: int,
    severity: str,
    counter: str,
    name: str,
    *,
    colored: bool,
    loc_width: int,
) -> str:
    location = f"{loc_path}:{loc_line}:{loc_col}: {severity}:"
    location = location.ljust(loc_width)
    counter_text = f"{WHITE}{counter}{RESET}" if colored else counter
    if colored:
        return f"{location} {BRACKET_COLOR}[{RESET}{counter_text}{BRACKET_COLOR}]{RESET} {name}"
    return f"{location} [{counter_text}] {name}"


def location_prefix_width(specs: Sequence[PropertySpec]) -> int:
    width = 0
    for spec in specs:
        loc_path = str(spec.path.resolve())
        for severity in ("note", "error"):
            width = max(width, len(f"{loc_path}:{spec.line}:1: {severity}:"))
    return width


def progress_prefix_width(specs: Sequence[PropertySpec], loc_width: int, total: int | None = None) -> int:
    total = total or len(specs)
    width = 0
    for index, spec in enumerate(specs, 1):
        loc_path = str(spec.path.resolve())
        counter = f"{index:04d}/{total:04d}"
        for severity in ("note", "error"):
            width = max(
                width,
                len(progress_prefix(loc_path, spec.line, 1, severity, counter, spec.name, colored=False, loc_width=loc_width)),
            )
    return width


def widths_for(specs: Sequence[PropertySpec], total: int | None = None) -> Widths:
    loc_width = location_prefix_width(specs)
    return Widths(location=loc_width, progress=progress_prefix_width(specs, loc_width, total or len(specs)))


def section_key(spec: PropertySpec | str) -> str:
    if isinstance(spec, str):
        name = spec
        section = spec
        path_text = ""
        tier = "stable"
    else:
        name = spec.name
        section = spec.section
        tier = spec.tier
        try:
            path_text = spec.path.relative_to(PROPERTY_ROOT).as_posix()
        except ValueError:
            path_text = spec.path.as_posix()

    text = f"{path_text} {section} {name}".lower()
    if tier == "oracle":
        return "oracle"
    if tier == "stress":
        return "stress"
    if tier == "compile-fail" or "negative" in text or "compile-fail" in text:
        return "negative"
    if text.startswith("00-core") or any(k in text for k in ("arithmetic", "comparison", "order", "lattice")):
        return "core"
    if text.startswith("10-control") or any(k in text for k in ("control", "if", "loop", "while", "for", "phi")):
        return "control"
    if text.startswith("20-binding") or any(k in text for k in ("binding", "with", "set!", "mutation", "env", "shadow")):
        return "binding"
    if text.startswith("30-codegen") or any(k in text for k in ("dispatch", "bitwise", "operator", "logic-coercion")):
        return "dispatch"
    if text.startswith("40-runtime") or any(k in text for k in ("array", "list", "set", "string", "collection")):
        return "runtime"
    if text.startswith("50-quote") or any(k in text for k in ("quote", "null", "path", "predicate", "conversion", "cast")):
        return "quote"
    if text.startswith("60-cross") or "cross" in text or "combo" in text or "interaction" in text:
        return "cross"
    return "misc"


def section_sort_key(section: str) -> tuple[int, str]:
    return (SECTION_ORDER.get(section, 1000), section)


def print_status(
    outcome: CaseOutcome,
    index: int,
    total: int,
    widths: Widths,
    *,
    show_expr: bool = False,
) -> None:
    result_ok = outcome.status in {"PASS", "XFAIL"}
    status = {
        "PASS": f"{BOLD_GREEN}PASS{RESET}",
        "FAIL": f"{BOLD_RED}FAIL{RESET}",
        "XFAIL": f"{YELLOW}XFAIL{RESET}",
        "XPASS": f"{BOLD_RED}XPASS{RESET}",
        "SKIP": f"{YELLOW}SKIP{RESET}",
    }.get(outcome.status, outcome.status)
    spec = outcome.generated.spec
    severity = "note" if result_ok else "error"
    loc_path, loc_line, loc_col = str(spec.path.resolve()), spec.line, 1
    counter = f"{index:04d}/{total:04d}"
    prefix = progress_prefix(loc_path, loc_line, loc_col, severity, counter, spec.name, colored=True, loc_width=widths.location)
    plain = progress_prefix(loc_path, loc_line, loc_col, severity, counter, spec.name, colored=False, loc_width=widths.location)
    pad = " " * max(2, widths.progress - len(plain) + 2)
    print(f"{prefix}{pad}{status} {format_duration(outcome.elapsed_ns)}")
    if not result_ok:
        print_failure_context(outcome, show_expr=show_expr)
    elif show_expr:
        print_expr_context(outcome)


def print_inventory_status(spec: PropertySpec, index: int, total: int, widths: Widths) -> None:
    counter = f"{index:04d}/{total:04d}"
    prefix = progress_prefix(str(spec.path.resolve()), spec.line, 1, "note", counter, spec.name, colored=True, loc_width=widths.location)
    plain = progress_prefix(str(spec.path.resolve()), spec.line, 1, "note", counter, spec.name, colored=False, loc_width=widths.location)
    pad = " " * max(2, widths.progress - len(plain) + 2)
    detail = f"{spec.tier}:{spec.kind} {spec.arg_summary} -> {spec.typ} expect={spec.expectation_summary}"
    print(f"{prefix}{pad}{YELLOW}READY{RESET} {format_duration(0)}  {GRAY}{detail}{RESET}")


def indent_output(output: str) -> str:
    return "\n".join(f"    {line}" for line in output.rstrip().splitlines())


def print_compiler_output(output: str, limit: int = 80) -> None:
    if not output.strip():
        return
    for line in output.rstrip().splitlines()[-limit:]:
        # Preserve compiler diagnostics at column 1 for Emacs.
        if re.match(r"^(?:/|[A-Za-z]:\\|[^\s:]+\.mon:)", line):
            print(line)
        else:
            print(f"    {line}")


def print_expr_context(outcome: CaseOutcome) -> None:
    gen = outcome.generated
    print(f"    expr:      {gen.source}")
    if gen.source != gen.source_before_rewrite:
        print(f"    before:    {gen.source_before_rewrite}")
    if gen.rewrites:
        print(f"    rewrite:   {'; '.join(gen.rewrites)}")
    if gen.bindings:
        print("    bindings:  " + ", ".join(f"{name}={expr.src}" for name, expr in gen.bindings.items()))


def print_failure_context(outcome: CaseOutcome, *, show_expr: bool) -> None:
    gen = outcome.generated
    if outcome.replay_path:
        try:
            replay = outcome.replay_path.relative_to(ROOT).as_posix()
        except ValueError:
            replay = outcome.replay_path.as_posix()
        print(f"    replay:   {replay}")
    print(f"    reason:   {outcome.message or outcome.status}")
    print(f"    phase:    {outcome.phase}")
    print(f"    expect:   {outcome.expected}")
    print(f"    actual:   {outcome.actual if outcome.actual is not None else '∅'}")
    print(f"    tier:     {gen.spec.tier}")
    print(f"    kind:     {gen.spec.kind}")
    print(f"    section:  {gen.spec.section}")
    if outcome.shrunk:
        print(f"    shrunk:   {outcome.shrunk.source}")
    if show_expr or not outcome.shrunk:
        print_expr_context(outcome)
    print_compiler_output(outcome.compiler_output)


### S-expression parsing and safe source normalization

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
    try:
        tree = parse_sexpr_tokens(tokenize_sexpr(src))
        rewrites: list[str] = []
        new_tree = rewrite_tree(tree, rewrites)
        return render_sexpr(new_tree), tuple(rewrites)
    except Exception:
        return src, ()


### Literal rendering and strategy-directed typed generation


def mon_bool(value: bool) -> str:
    return "True" if bool(value) else "False"


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


def int_edge(rng: random.Random) -> int:
    return int(rng.choice([-128, -97, -64, -33, -17, -13, -12, -8, -3, -2, -1, 0, 1, 2, 3, 7, 8, 12, 13, 17, 31, 32, 63, 64, 97, 128]))


def int_lit(rng: random.Random, strategies: Sequence[str] = ()) -> Expr:
    if "tiny" in strategies:
        value = rng.choice([-2, -1, 0, 1, 2])
    elif "nat" in strategies or "index" in strategies:
        value = rng.choice([0, 1, 2, 3, 4, 5, 8, 13]) if rng.random() < 0.7 else rng.randint(0, 24)
    elif "edge" in strategies or rng.random() < 0.55:
        value = int_edge(rng)
    else:
        value = rng.randint(-128, 128)
    return Expr("Int", str(value), int(value))


def nat_lit(rng: random.Random, strategies: Sequence[str] = ()) -> Expr:
    value = rng.choice([0, 1, 2, 3, 4, 5, 8, 13]) if rng.random() < 0.7 else rng.randint(0, 32)
    return Expr("Nat", str(value), int(value))


def float_lit(rng: random.Random, strategies: Sequence[str] = ()) -> Expr:
    value = rng.choice([-3.5, -1.0, -0.5, 0.0, 0.5, 1.0, 2.25, 10.0]) if rng.random() < 0.7 else rng.uniform(-20, 20)
    src = f"{value:.6g}"
    if "." not in src and "e" not in src.lower():
        src += ".0"
    return Expr("Float", src, float(value))


def gen_bool(rng: random.Random, depth: int, strategies: Sequence[str] = ()) -> Expr:
    if depth <= 0 or rng.random() < 0.16:
        value = bool(rng.getrandbits(1))
        return Expr("Bool", mon_bool(value), value)

    choices = ["not", "eq", "ne", "lt", "lte", "gt", "gte", "if", "do", "id"]
    if "logic" in strategies:
        choices += ["and", "or", "nested-logic"] * 2
    if "predicate" in strategies:
        choices += ["predicate-int", "predicate-bool"] * 2
    if "if" in strategies or "phi" in strategies:
        choices += ["if"] * 3
    choice = rng.choice(choices)

    if choice == "not":
        x = gen_bool(rng, depth - 1, strategies)
        return Expr("Bool", f"(not {x.src})", not bool(x.value))
    if choice == "and":
        a = gen_bool(rng, depth - 1, strategies)
        b = gen_bool(rng, depth - 1, strategies)
        return Expr("Bool", f"(and {a.src} {b.src})", bool(a.value and b.value))
    if choice == "or":
        a = gen_bool(rng, depth - 1, strategies)
        b = gen_bool(rng, depth - 1, strategies)
        return Expr("Bool", f"(or {a.src} {b.src})", bool(a.value or b.value))
    if choice == "nested-logic":
        a = gen_bool(rng, depth - 1, strategies)
        b = gen_bool(rng, depth - 1, strategies)
        c = gen_bool(rng, depth - 1, strategies)
        return Expr("Bool", f"(and (or {a.src} {b.src}) (not {c.src}))", bool((a.value or b.value) and (not c.value)))
    if choice == "id":
        x = gen_bool(rng, depth - 1, strategies)
        return Expr("Bool", f"(fuzz-bool-id? {x.src})", bool(x.value))
    if choice in {"eq", "ne", "lt", "lte", "gt", "gte"}:
        a = gen_int(rng, depth - 1, ())
        b = gen_int(rng, depth - 1, ())
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
    if choice == "predicate-int":
        x = gen_int(rng, depth - 1, ())
        return Expr("Bool", f"(int? {x.src})", True)
    if choice == "predicate-bool":
        x = gen_bool(rng, depth - 1, ())
        return Expr("Bool", f"(bool? {x.src})", True)
    if choice == "if":
        c = gen_bool(rng, depth - 1, ())
        t = gen_bool(rng, depth - 1, strategies)
        e = gen_bool(rng, depth - 1, strategies)
        return Expr("Bool", f"(if {c.src} {t.src} {e.src})", bool(t.value if c.value else e.value))

    setup = gen_int(rng, depth - 1, ())
    body = gen_bool(rng, depth - 1, strategies)
    return Expr("Bool", f"(do {setup.src} {body.src})", bool(body.value))


def gen_int_with_var(rng: random.Random, depth: int, name: str, value: int, strategies: Sequence[str] = ()) -> Expr:
    if depth <= 0 or rng.random() < 0.30:
        return Expr("Int", name, value)
    other = gen_int(rng, depth - 1, strategies)
    op_src, op_fn = rng.choice([
        ("fuzz-add", operator.add),
        ("fuzz-sub", operator.sub),
        ("fuzz-mul", operator.mul),
    ])
    if rng.getrandbits(1):
        return Expr("Int", f"({op_src} {name} {other.src})", int(op_fn(value, int(other.value))))
    return Expr("Int", f"({op_src} {other.src} {name})", int(op_fn(int(other.value), value)))


def gen_int(rng: random.Random, depth: int, strategies: Sequence[str] = ()) -> Expr:
    if depth <= 0:
        return int_lit(rng, strategies)

    choices = ["lit", "id", "add", "sub", "mul", "if", "with", "do", "min", "max"]
    if "bitwise" in strategies:
        choices += ["bit-and", "bit-or", "bit-xor", "shift-left", "shift-right"] * 2
    if "mod" in strategies:
        choices += ["mod"] * 2
    if "if" in strategies or "phi" in strategies:
        choices += ["if"] * 3
    if "with" in strategies or "env" in strategies:
        choices += ["with"] * 3
    if "do" in strategies:
        choices += ["do"] * 3
    if "array" in strategies:
        choices += ["array-index"] * 2
    choice = rng.choice(choices)

    if choice == "lit":
        return int_lit(rng, strategies)
    if choice == "id":
        x = gen_int(rng, depth - 1, strategies)
        return Expr("Int", f"(fuzz-id-int {x.src})", int(x.value))
    if choice in {"add", "sub", "mul"}:
        a = gen_int(rng, depth - 1, strategies)
        b = gen_int(rng, depth - 1, strategies)
        op_src = {"add": "fuzz-add", "sub": "fuzz-sub", "mul": "fuzz-mul"}[choice]
        op_fn = {"add": operator.add, "sub": operator.sub, "mul": operator.mul}[choice]
        return Expr("Int", f"({op_src} {a.src} {b.src})", int(op_fn(int(a.value), int(b.value))))
    if choice == "min":
        a = gen_int(rng, depth - 1, strategies)
        b = gen_int(rng, depth - 1, strategies)
        return Expr("Int", f"(fuzz-min {a.src} {b.src})", min(int(a.value), int(b.value)))
    if choice == "max":
        a = gen_int(rng, depth - 1, strategies)
        b = gen_int(rng, depth - 1, strategies)
        return Expr("Int", f"(fuzz-max {a.src} {b.src})", max(int(a.value), int(b.value)))
    if choice == "mod":
        a = gen_int(rng, depth - 1, strategies)
        b_value = rng.choice([1, 2, 3, 5, 7, 13])
        b = Expr("Int", str(b_value), b_value)
        return Expr("Int", f"(% {a.src} {b.src})", int(a.value) % int(b.value))
    if choice in {"bit-and", "bit-or", "bit-xor"}:
        a = gen_int(rng, depth - 1, ("tiny",))
        b = gen_int(rng, depth - 1, ("tiny",))
        op_src = {"bit-and": "&", "bit-or": "bit-or", "bit-xor": "bit-xor"}[choice]
        op_fn = {"bit-and": operator.and_, "bit-or": operator.or_, "bit-xor": operator.xor}[choice]
        return Expr("Int", f"({op_src} {a.src} {b.src})", int(op_fn(int(a.value), int(b.value))))
    if choice in {"shift-left", "shift-right"}:
        a = gen_int(rng, depth - 1, ("tiny",))
        n = rng.choice([0, 1, 2, 3])
        if choice == "shift-left":
            return Expr("Int", f"(<< {a.src} {n})", int(a.value) << n)
        return Expr("Int", f"(>> {a.src} {n})", int(a.value) >> n)
    if choice == "array-index":
        values = [int_lit(rng, ("tiny",)).value for _ in range(rng.randint(1, 5))]
        idx = rng.randrange(0, len(values))
        return Expr("Int", f"({arr_src(values)} {idx})", int(values[idx]))
    if choice == "if":
        c = gen_bool(rng, depth - 1, ())
        t = gen_int(rng, depth - 1, strategies)
        e = gen_int(rng, depth - 1, strategies)
        return Expr("Int", f"(if {c.src} {t.src} {e.src})", int(t.value if c.value else e.value))
    if choice == "with":
        bound = gen_int(rng, depth - 1, strategies)
        body = gen_int_with_var(rng, depth - 1, "x", int(bound.value), strategies)
        return Expr("Int", f"(with [x {bound.src}] {body.src})", int(body.value))

    setup = gen_bool(rng, depth - 1, ()) if rng.random() < 0.25 else gen_int(rng, depth - 1, strategies)
    body = gen_int(rng, depth - 1, strategies)
    return Expr("Int", f"(do {setup.src} {body.src})", int(body.value))


def gen_string(rng: random.Random, depth: int, strategies: Sequence[str] = (), *, non_empty: bool = False) -> Expr:
    atoms = ["", "a", "b", "monadc", "nad", "hello", "with space", "abcabc", "_x_", "123", "quote\"x", "line\\ntext"]
    if non_empty:
        atoms = [s for s in atoms if s]
    if "escape" in strategies:
        value = rng.choice(["quote\"x", "slash\\x", "tab\ttext", "line\ntext"])
    elif rng.random() < 0.45:
        value = rng.choice(atoms)
    else:
        alphabet = "abcxyz012 _-"
        lo = 1 if non_empty else 0
        value = "".join(rng.choice(alphabet) for _ in range(rng.randint(lo, 16)))
    return Expr("String", string_lit(value), value)


def gen_char(rng: random.Random, depth: int, strategies: Sequence[str] = ()) -> Expr:
    value = rng.choice(list("abcxyz012 _-") + ["\n", "\t"])
    return Expr("Char", char_lit(value), value)


def gen_keyword(rng: random.Random, depth: int, strategies: Sequence[str] = ()) -> Expr:
    value = rng.choice([":a", ":b", ":monadc", ":fuzz", ":x-y", ":_k"])
    return Expr("Keyword", value, value)


def gen_path(rng: random.Random, depth: int, strategies: Sequence[str] = ()) -> Expr:
    value = rng.choice([
        "./context",
        "./tests/fuzzing",
        "../monadc",
        "/tmp/monadc-fuzz",
        "~/xos/projects/c/monadc/context",
    ])
    return Expr("Path", value, value)


def arr_src(values: Sequence[int], *, heap: bool = False) -> str:
    return ("~" if heap else "") + "[" + " ".join(str(v) for v in values) + "]"


def gen_arr_int(rng: random.Random, depth: int, strategies: Sequence[str] = (), *, heap: bool = False, non_empty: bool = False) -> Expr:
    lo = 1 if non_empty else 0
    if "small" in strategies:
        hi = 3
    else:
        hi = 6
    n = rng.randint(lo, hi)
    values = [int_lit(rng, ("tiny",)).value for _ in range(n)]
    typ = "HeapArrInt" if heap else "ArrInt"
    return Expr(typ, arr_src(values, heap=heap), tuple(int(v) for v in values))


def gen_list_int(rng: random.Random, depth: int, strategies: Sequence[str] = (), *, non_empty: bool = False) -> Expr:
    lo = 1 if non_empty else 0
    n = rng.randint(lo, 5)
    values = [int_lit(rng, ("tiny",)).value for _ in range(n)]
    if not values:
        return Expr("ListInt", "[]", tuple())
    return Expr("ListInt", "[" + " ".join(str(v) for v in values) + "]", tuple(int(v) for v in values))


def gen_dependent_arg(rng: random.Random, typ: str, depth: int, bindings: Mapping[str, Expr]) -> Expr | None:
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


def gen_arg(rng: random.Random, arg: ArgSpec, depth: int, bindings: Mapping[str, Expr]) -> Expr:
    typ = arg.typ.strip()
    strategies = arg.strategies
    dependent = gen_dependent_arg(rng, typ, depth, bindings)
    if dependent is not None:
        return dependent

    if typ in {"Int", "Integer"}:
        return gen_int(rng, depth, strategies)
    if typ == "Nat":
        return nat_lit(rng, strategies)
    if typ == "Bool":
        return gen_bool(rng, depth, strategies)
    if typ in {"Float", "Double"}:
        return float_lit(rng, strategies)
    if typ == "String":
        return gen_string(rng, depth, strategies)
    if typ == "NonEmptyString":
        return gen_string(rng, depth, strategies, non_empty=True)
    if typ == "Char":
        return gen_char(rng, depth, strategies)
    if typ == "Keyword":
        return gen_keyword(rng, depth, strategies)
    if typ == "Path":
        return gen_path(rng, depth, strategies)
    if typ in {"ArrInt", "ArrayInt", "Arr[Int]"}:
        return gen_arr_int(rng, depth, strategies)
    if typ in {"NonEmptyArrInt", "NonEmptyArrayInt"}:
        return gen_arr_int(rng, depth, strategies, non_empty=True)
    if typ in {"HeapArrInt", "HeapArrayInt"}:
        return gen_arr_int(rng, depth, strategies, heap=True)
    if typ in {"NonEmptyHeapArrInt", "NonEmptyHeapArrayInt"}:
        return gen_arr_int(rng, depth, strategies, heap=True, non_empty=True)
    if typ in {"ListInt", "List[Int]"}:
        return gen_list_int(rng, depth, strategies)
    if typ == "NonEmptyListInt":
        return gen_list_int(rng, depth, strategies, non_empty=True)
    raise ValueError(f"unsupported fuzz argument type {typ!r}")


### Property parsing and discovery

PLACEHOLDER_RE = re.compile(r"\{([A-Za-z_][A-Za-z0-9_]*)\}")


def parse_bool(value: str) -> bool:
    if value.lower() in {"true", "yes", "1"}:
        return True
    if value.lower() in {"false", "no", "0"}:
        return False
    raise ValueError(f"expected boolean, got {value!r}")


def parse_arg(item: str, path: Path, line_number: int) -> ArgSpec:
    if ":" not in item:
        raise ValueError(f"{path}:{line_number}: bad arg {item!r}; expected name:Type[@strategy+strategy]")
    name, spec = item.split(":", 1)
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise ValueError(f"{path}:{line_number}: bad arg name {name!r}")
    if "@" in spec:
        typ, raw_strategies = spec.split("@", 1)
        strategies = tuple(s for s in re.split(r"[+,]", raw_strategies) if s)
    else:
        typ = spec
        strategies = ()
    if not typ:
        raise ValueError(f"{path}:{line_number}: missing type for arg {name!r}")
    return ArgSpec(name, typ, strategies)


def split_csv(value: str) -> tuple[str, ...]:
    return tuple(item.strip() for item in re.split(r"[,\s]+", value) if item.strip())


def load_property(path: Path, *, disabled: bool = False) -> PropertySpec:
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

    required = ["name", "section", "args", "type", "description"]
    missing = [key for key in required if key not in data]
    if missing:
        raise ValueError(f"{path}: missing keys: {', '.join(missing)}")

    kind = data.get("kind", "law").strip() or "law"
    if kind not in VALID_KINDS:
        raise ValueError(f"{path}:{line_numbers.get('kind', 1)}: invalid kind {kind!r}; expected one of {sorted(VALID_KINDS)}")

    tier = data.get("tier", "compile-fail" if kind == "compile-fail" else "stable").strip() or "stable"
    if tier not in VALID_TIERS:
        raise ValueError(f"{path}:{line_numbers.get('tier', 1)}: invalid tier {tier!r}; expected one of {sorted(VALID_TIERS)}")

    law = data.get("law")
    program = data.get("program")
    if kind == "law" and law is None:
        raise ValueError(f"{path}: kind law requires law:")
    if kind == "program" and program is None:
        raise ValueError(f"{path}: kind program requires program:")
    if kind == "compile-fail" and law is None and program is None:
        raise ValueError(f"{path}: kind compile-fail requires law: or program:")

    if kind == "compile-fail":
        if "expect-diagnostic" not in data:
            raise ValueError(f"{path}: compile-fail property requires expect-diagnostic:")
    elif "expect" not in data and "expect-python" not in data:
        raise ValueError(f"{path}: missing one of: expect, expect-python")

    args = tuple(parse_arg(item, path, line_numbers.get("args", 1)) for item in data.get("args", "").split())
    declared = {arg.name for arg in args}
    used = set(PLACEHOLDER_RE.findall((law or "") + " " + (program or "")))
    undeclared = sorted(used - declared)
    if undeclared:
        line = line_numbers.get("law", line_numbers.get("program", 1))
        raise ValueError(f"{path}:{line}: undeclared placeholder(s): {', '.join(undeclared)}")

    line = line_numbers.get("law", line_numbers.get("program", 1))
    features = split_csv(data.get("features", ""))
    if not features:
        features = infer_features(data.get("section", ""), law or program or "")

    return PropertySpec(
        name=data["name"],
        section=data["section"],
        tier=tier,
        kind=kind,
        args=args,
        typ=data["type"],
        expect=data.get("expect"),
        expect_python=data.get("expect-python"),
        expect_diagnostic=data.get("expect-diagnostic"),
        description=data["description"],
        law=law,
        program=program,
        features=features,
        path=path,
        line=line,
        xfail=parse_bool(data.get("xfail", "False")),
        disabled=disabled,
    )


def infer_features(section: str, source: str) -> tuple[str, ...]:
    features: set[str] = set()
    text = f"{section} {source}".lower()
    probes = [
        ("if", "if"), ("do", "do"), ("begin", "begin"), ("when", "when"), ("unless", "unless"),
        ("while", "while"), ("for", "for"), ("with", "with"), ("set!", "set!"),
        ("array", "["), ("heap-array", "~["), ("list", "list"), ("set", "set"),
        ("string", "string"), ("quote", "quote"), ("quasiquote", "quasiquote"),
        ("null", "nil"), ("path", "path"), ("bitwise", "bit"), ("mod", "%"),
        ("predicate", "?"), ("logic", "and"), ("logic", "or"), ("comparison", "<"),
        ("arithmetic", "+"), ("arithmetic", "-"), ("arithmetic", "*"),
    ]
    for feature, needle in probes:
        if needle in text:
            features.add(feature)
    if not features:
        features.add(section.split("-", 1)[0] if section else "misc")
    return tuple(sorted(features))


def load_properties(include_disabled: bool = False) -> tuple[list[PropertySpec], list[str]]:
    paths = sorted(
        (path for path in PROPERTY_ROOT.rglob("*.fuzz") if path.is_file()),
        key=lambda p: p.relative_to(PROPERTY_ROOT).as_posix(),
    )
    if include_disabled:
        paths += sorted(
            (path for path in PROPERTY_ROOT.rglob("*.fuzz.disabled") if path.is_file()),
            key=lambda p: p.relative_to(PROPERTY_ROOT).as_posix(),
        )
    if not paths:
        raise SystemExit(f"no fuzz property files found recursively in {PROPERTY_ROOT}")

    specs: list[PropertySpec] = []
    seen: dict[str, Path] = {}
    errors: list[str] = []
    for path in paths:
        try:
            disabled = path.name.endswith(".disabled")
            spec = load_property(path, disabled=disabled)
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
    return specs, errors


def select_properties(specs: Sequence[PropertySpec], cli: Cli) -> list[PropertySpec]:
    selected = [spec for spec in specs if not spec.disabled or cli.include_disabled]
    if cli.tiers and "all" not in cli.tiers:
        selected = [spec for spec in selected if spec.tier in cli.tiers]
    if cli.properties:
        by_name = {spec.name: spec for spec in selected}
        missing = sorted(cli.properties - set(by_name))
        if missing:
            raise SystemExit(f"unknown fuzz properties: {', '.join(missing)}")
        selected = [by_name[name] for name in sorted(cli.properties)]
    if cli.sections:
        selected = [spec for spec in selected if spec.section in cli.sections or section_key(spec) in cli.sections]
        if not selected:
            raise SystemExit(f"no properties selected for sections: {', '.join(sorted(cli.sections))}")
    return selected


### Oracle and generated source

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
    if spec.kind == "compile-fail":
        return spec.expect_diagnostic or "compile failed"
    if spec.expect_python:
        local = {name: expr.value for name, expr in bindings.items()}
        value = eval(spec.expect_python, SAFE_ORACLE_GLOBALS, local)  # noqa: S307 - local test oracle by design.
        if isinstance(value, bool):
            return mon_bool(value)
        return str(value)
    assert spec.expect is not None
    return spec.expect


def substitute_placeholders(template: str, bindings: Mapping[str, Expr]) -> str:
    out = template
    for name, expr in bindings.items():
        out = out.replace("{" + name + "}", expr.src)
    return out


def generate_case(spec: PropertySpec, rng: random.Random, seed: int, ordinal: int, depth: int) -> GeneratedCase:
    start = time.perf_counter_ns()
    bindings: dict[str, Expr] = {}
    for arg in spec.args:
        bindings[arg.name] = gen_arg(rng, arg, depth, bindings)

    source_template = spec.program if spec.kind == "program" and spec.program is not None else spec.program or spec.law or "False"
    source = substitute_placeholders(source_template, bindings)
    before = source
    rewrites: tuple[str, ...] = ()
    if spec.kind == "law":
        source, rewrites = normalize_law_source(source)
    expected = expected_show(spec, bindings)
    elapsed = time.perf_counter_ns() - start
    return GeneratedCase(spec, seed, ordinal, source, before, expected, bindings, rewrites, elapsed)


def rebuild_case_with_bindings(case: GeneratedCase, bindings: Mapping[str, Expr]) -> GeneratedCase:
    spec = case.spec
    source_template = spec.program if spec.kind == "program" and spec.program is not None else spec.program or spec.law or "False"
    source = substitute_placeholders(source_template, bindings)
    before = source
    rewrites: tuple[str, ...] = ()
    if spec.kind == "law":
        source, rewrites = normalize_law_source(source)
    expected = expected_show(spec, bindings)
    return GeneratedCase(spec, case.seed, case.ordinal, source, before, expected, dict(bindings), rewrites, case.gen_elapsed_ns)


def coverage_cases(specs: Sequence[PropertySpec], seed: int, depth: int) -> list[GeneratedCase]:
    cases: list[GeneratedCase] = []
    for i, spec in enumerate(specs, 1):
        rng = random.Random((seed * 1_000_003) + i)
        cases.append(generate_case(spec, rng, seed, i, depth))
    return cases


def random_batch(seed: int, laws_per_case: int, depth: int, specs: Sequence[PropertySpec]) -> list[GeneratedCase]:
    rng = random.Random(seed)
    eligible = [spec for spec in specs if spec.kind == "law" and not spec.xfail]
    if not eligible:
        return []
    return [generate_case(rng.choice(eligible), rng, seed, i, depth) for i in range(1, laws_per_case + 1)]


### Program construction, execution, replay


def module_header(seed: int) -> list[str]:
    return [
        ";; generated by tests/fuzzing/fuzz_codegen.py",
        f";; seed: {seed}",
        "(module Main)",
        "",
        STABLE_DEFS.rstrip(),
        "",
    ]


def build_program(seed: int, cases: Sequence[GeneratedCase]) -> tuple[str, str]:
    lines = module_header(seed)
    expected: list[str] = []
    for index, case in enumerate(cases, 1):
        spec = case.spec
        lines.append(f";; case {index}: {spec.name} from {spec.relpath}:{spec.line}")
        for rewrite in case.rewrites:
            lines.append(f";; rewrite: {rewrite}")
        if spec.kind == "program":
            lines.append(case.source)
        else:
            lines.append(f"(show {case.source})")
        if spec.kind != "compile-fail":
            expected.append(case.expected)
    return "\n".join(lines) + "\n", "\n".join(expected) + ("\n" if expected else "")


def ensure_last_root() -> Path:
    LAST_ROOT.mkdir(parents=True, exist_ok=True)
    return LAST_ROOT


def save_replay(label: str, seed: int, source: str, expected: str, actual: str, compiler_output: str = "") -> Path:
    out = ensure_last_root()
    safe_label = safe_name(label).strip("_")[:96] or "fuzz"
    stem = f"{safe_label}_seed_{seed:06d}"
    src = out / f"{stem}.mon"
    src.write_text(source, encoding="utf-8")
    (out / f"{stem}.expected").write_text(expected, encoding="utf-8")
    (out / f"{stem}.actual").write_text(actual, encoding="utf-8")
    if compiler_output:
        (out / f"{stem}.compiler").write_text(compiler_output, encoding="utf-8")
    return src


def compile_and_run(seed: int, cases: Sequence[GeneratedCase], *, keep: bool, label: str) -> tuple[ExecutionResult, str, str, Path | None]:
    source, expected = build_program(seed, cases)
    with tempfile.TemporaryDirectory(prefix=f"monadc-fuzz-{safe_name(label)}-{seed}-") as tmp_name:
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
        if not exe.exists() or not exe.stat().st_mode & 0o111:
            message = f"expected executable at {exe}"
            replay = save_replay(label, seed, source, expected, compile_result.stdout, compile_result.stdout)
            return (
                ExecutionResult(False, "compile", 0, compile_ns, 0, "", compile_result.stdout, message),
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


def compile_and_run_at(seed: int, cases: Sequence[GeneratedCase], opt_flag: str, *, label: str) -> tuple[int, str, str]:
    source, expected = build_program(seed, cases)
    with tempfile.TemporaryDirectory(prefix=f"monadc-fuzz-{safe_name(label)}-{seed}-") as tmp_name:
        tmp = Path(tmp_name)
        src = tmp / f"fuzz_{seed}.mon"
        exe = tmp / f"fuzz_{seed}"
        src.write_text(source, encoding="utf-8")
        compile_result = subprocess.run(
            [str(MONAD), str(src), opt_flag, "-o", str(exe)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if compile_result.returncode != 0:
            return compile_result.returncode, "", compile_result.stdout
        if not exe.exists() or not exe.stat().st_mode & 0o111:
            return 0, "", compile_result.stdout
        run_result = subprocess.run(
            [str(exe)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        return run_result.returncode, run_result.stdout, compile_result.stdout


def compile_and_run_diff(seed: int, cases: Sequence[GeneratedCase], *, keep: bool, label: str) -> tuple[ExecutionResult, str, str, Path | None]:
    source, expected = build_program(seed, cases)
    start = time.perf_counter_ns()
    rc_a, out_a, log_a = compile_and_run_at(seed, cases, "-O0", label=f"{label}_O0")
    rc_b, out_b, log_b = compile_and_run_at(seed, cases, "-O2", label=f"{label}_O2")
    elapsed = time.perf_counter_ns() - start

    ok = rc_a == rc_b and out_a == out_b
    if rc_a != 0 and rc_b != 0:
        message = "ok"
        ok = True
    elif not ok:
        message = "optimization levels disagree (-O0 vs -O2)"
    else:
        message = "ok"

    combined_log = ""
    replay = None
    if keep or not ok:
        combined_log = (
            f"-O0 (rc={rc_a}):\n{log_a}\n--- stdout ---\n{out_a}\n"
            f"-O2 (rc={rc_b}):\n{log_b}\n--- stdout ---\n{out_b}\n"
        )
        actual_text = f"-O0: rc={rc_a} stdout={out_a!r}\n-O2: rc={rc_b} stdout={out_b!r}\n"
        replay = save_replay(label, seed, source, expected, actual_text, combined_log)

    return (
        ExecutionResult(ok, "diff", rc_b, elapsed, 0, out_b, combined_log, message),
        source,
        f"-O0: rc={rc_a} stdout={out_a!r}",
        replay,
    )


def compile_only(seed: int, case: GeneratedCase, *, keep: bool, label: str) -> tuple[ExecutionResult, str, str, Path | None]:
    source, expected = build_program(seed, [case])
    diagnostic = case.expected
    with tempfile.TemporaryDirectory(prefix=f"monadc-fuzz-{safe_name(label)}-{seed}-") as tmp_name:
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
        ok = compile_result.returncode != 0 and (not diagnostic or diagnostic in compile_result.stdout)
        message = "expected compile failure observed" if ok else "compile-fail expectation did not match"
        replay = None
        if keep or not ok:
            replay = save_replay(label, seed, source, diagnostic + "\n", compile_result.stdout, compile_result.stdout)
        return (
            ExecutionResult(ok, "compile", compile_result.returncode, compile_ns, 0, compile_result.stdout, compile_result.stdout, message),
            source,
            diagnostic,
            replay,
        )


def run_isolated_case(case: GeneratedCase, *, keep: bool, label: str) -> CaseOutcome:
    spec = case.spec
    if spec.kind == "compile-fail":
        result, _source, expected, replay = compile_only(case.seed, case, keep=keep, label=f"{label}_{spec.name}")
        actual = result.output.strip().splitlines()[-1] if result.output.strip().splitlines() else None
        ok = result.ok
    else:
        result, _source, expected, replay = compile_and_run(case.seed, [case], keep=keep, label=f"{label}_{spec.name}")
        actual = result.stdout.splitlines()[0] if result.stdout.splitlines() else None
        ok = result.ok and actual == case.expected

    if spec.xfail:
        status = "XFAIL" if not ok else "XPASS"
    else:
        status = "PASS" if ok else "FAIL"
    elapsed = result.compile_ns + result.run_ns
    return CaseOutcome(
        generated=case,
        ok=ok,
        status=status,
        phase=result.phase,
        actual=actual,
        expected=case.expected,
        elapsed_ns=elapsed,
        replay_path=replay,
        message=result.message,
        compiler_output=result.output if not ok else "",
    )


def isolate_batch_failures(cases: Sequence[GeneratedCase], *, keep: bool, label: str) -> list[CaseOutcome]:
    return [run_isolated_case(case, keep=keep, label=label) for case in cases]


### Shrinking


def simpler_exprs(expr: Expr) -> list[Expr]:
    typ = expr.typ
    if typ in {"Int", "Integer", "Nat"}:
        candidates = [0, 1, -1, 2, -2]
        return [Expr(typ, str(v), v) for v in candidates if v != expr.value]
    if typ == "Bool":
        return [Expr("Bool", mon_bool(not bool(expr.value)), not bool(expr.value))]
    if typ in {"String", "NonEmptyString"}:
        vals = ["", "a", str(expr.value)[:1], str(expr.value)[:2]]
        return [Expr("String", string_lit(v), v) for v in dict.fromkeys(vals) if v != expr.value]
    if typ in {"ArrInt", "HeapArrInt", "ListInt"}:
        vals = [tuple(), (0,), (1,), tuple(expr.value[:1]) if isinstance(expr.value, tuple) else tuple()]
        out: list[Expr] = []
        for v in dict.fromkeys(vals):
            if v == expr.value:
                continue
            heap = typ == "HeapArrInt"
            src = arr_src(v, heap=heap) if typ != "ListInt" else "[" + " ".join(str(x) for x in v) + "]"
            out.append(Expr(typ, src, v))
        return out
    return []


def failure_signature(outcome: CaseOutcome) -> str:
    text = outcome.compiler_output or outcome.message or outcome.status
    for line in text.splitlines():
        if ": error:" in line or ": warning:" in line or "error:" in line:
            return re.sub(r"/[^\s:]+", "<path>", line.strip())[:200]
    return (outcome.message or outcome.status)[:200]


def try_shrink(outcome: CaseOutcome, *, max_steps: int) -> CaseOutcome:
    if outcome.ok or outcome.generated.spec.kind == "program" or max_steps <= 0:
        return outcome
    current = outcome.generated
    current_sig = failure_signature(outcome)
    steps = 0
    improved = True
    while improved and steps < max_steps:
        improved = False
        for name, expr in list(current.bindings.items()):
            for candidate in simpler_exprs(expr):
                if steps >= max_steps:
                    break
                new_bindings = dict(current.bindings)
                new_bindings[name] = candidate
                try:
                    trial = rebuild_case_with_bindings(current, new_bindings)
                except Exception:
                    continue
                trial_outcome = run_isolated_case(trial, keep=True, label=f"shrink_{current.spec.name}")
                steps += 1
                if not trial_outcome.ok and failure_signature(trial_outcome) == current_sig:
                    current = trial
                    outcome = dataclasses.replace(trial_outcome, shrunk=trial)
                    improved = True
                    break
            if improved or steps >= max_steps:
                break
    return dataclasses.replace(outcome, shrunk=current if current is not outcome.generated else outcome.shrunk)


### Runner

class FuzzRunner:
    def __init__(self, specs: Sequence[PropertySpec], cli: Cli) -> None:
        self.specs = list(specs)
        self.cli = cli
        self.total = len(specs)
        self.results: list[CaseOutcome] = []
        self.failures: list[CaseOutcome] = []
        self.first_failure: CaseOutcome | None = None
        self.current_section: str | None = None
        self.section_stats: dict[str, SectionStats] = {}
        self.feature_stats: dict[str, SectionStats] = {}
        self.section_widths: dict[str, Widths] = self.compute_section_widths(self.specs)
        self._live_line_active = False
        self._live_line_width = 0
        for spec in self.specs:
            self.section_stats.setdefault(section_key(spec), SectionStats()).total += 1

    def compute_section_widths(self, specs: Sequence[PropertySpec]) -> dict[str, Widths]:
        grouped: dict[str, list[PropertySpec]] = defaultdict(list)
        for spec in specs:
            grouped[section_key(spec)].append(spec)
        return {section: widths_for(group, len(specs)) for section, group in grouped.items()}

    def begin_section(self, section: str) -> None:
        if section != self.current_section:
            self.clear_live_line()
            if self.current_section is not None:
                self.print_section_footer(self.current_section)
            self.current_section = section
            print_section_header(section)

    def clear_live_line(self) -> None:
        if self._live_line_active:
            sys.stdout.write("\r" + " " * self._live_line_width + "\r")
            sys.stdout.flush()
            self._live_line_active = False

    def print_live_progress(self, spec: PropertySpec, index: int, total: int) -> None:
        color, title = SECTION_STYLES.get(section_key(spec), (YELLOW, section_key(spec).title()))
        line = f"  {title}: running {index}/{total} ({spec.relpath}:{spec.line})"
        pad = " " * max(0, self._live_line_width - len(line))
        sys.stdout.write("\r" + line + pad)
        sys.stdout.flush()
        self._live_line_width = max(self._live_line_width, len(line))
        self._live_line_active = True

    def print_section_footer(self, section: str) -> None:
        self.clear_live_line()
        stats = self.section_stats.get(section, SectionStats())
        elapsed = format_duration(stats.elapsed_ns)
        color, title = SECTION_STYLES.get(section, (YELLOW, section.title()))
        ok_count = stats.passed + stats.xfailed
        bad_count = stats.failed + stats.xpassed
        summary = f"{title}: {ok_count}/{stats.total} ok ({stats.percent:5.1f}%) | {bad_count} failed | {strip_ansi(elapsed)}"
        print_section_line(summary, color, bold=True)

    def record(self, outcome: CaseOutcome, index: int, *, show: bool) -> None:
        self.results.append(outcome)
        if outcome.status in {"FAIL", "XPASS"}:
            self.failures.append(outcome)
            if self.first_failure is None:
                self.first_failure = outcome
        section = section_key(outcome.generated.spec)
        stats = self.section_stats.setdefault(section, SectionStats())
        if outcome.status == "PASS":
            stats.passed += 1
        elif outcome.status == "FAIL":
            stats.failed += 1
        elif outcome.status == "XFAIL":
            stats.xfailed += 1
        elif outcome.status == "XPASS":
            stats.xpassed += 1
        stats.elapsed_ns += outcome.elapsed_ns
        for feature in outcome.generated.spec.features:
            fstats = self.feature_stats.setdefault(feature, SectionStats())
            fstats.total += 1
            if outcome.status == "PASS":
                fstats.passed += 1
            elif outcome.status == "FAIL":
                fstats.failed += 1
            elif outcome.status == "XFAIL":
                fstats.xfailed += 1
            elif outcome.status == "XPASS":
                fstats.xpassed += 1
            fstats.elapsed_ns += outcome.elapsed_ns
        if show:
            self.clear_live_line()
            widths = self.section_widths.get(section, widths_for([outcome.generated.spec], self.total))
            print_status(outcome, index, self.total, widths, show_expr=self.cli.show_expr or outcome.status != "PASS")


### Reporting helpers


def render_config(cli: Cli, specs: Sequence[PropertySpec], selected: Sequence[PropertySpec], errors: Sequence[str]) -> None:
    tiers = Counter(spec.tier for spec in selected)
    kinds = Counter(spec.kind for spec in selected)
    broad_sections = Counter(section_key(spec) for spec in selected)
    arg_types = Counter(arg.typ for spec in selected for arg in spec.args)
    nested = sum(1 for spec in selected if spec.path.relative_to(PROPERTY_ROOT).as_posix().count("/") > 0)
    rewritable = sum(1 for spec in selected if spec.law and re.search(r"\(\s*=\s+[^()]+\s+[^()]+\s+[^()]+", spec.law))

    print()
    print(f"Total fuzz properties registered: {len(selected)}".center(WIDTH + 2))
    print()
    print_rule("FUZZ CONFIG")
    print()
    rows = [
        ("root", ROOT.as_posix()),
        ("properties", f"{len(selected)}/{len(specs)} selected from {PROPERTY_ROOT.relative_to(ROOT).as_posix()}"),
        ("recursive", f"yes; nested={nested}"),
        ("tiers", ", ".join(f"{k}={v}" for k, v in sorted(tiers.items())) or "none"),
        ("kinds", ", ".join(f"{k}={v}" for k, v in sorted(kinds.items())) or "none"),
        ("sections", ", ".join(f"{k}={v}" for k, v in sorted(broad_sections.items(), key=lambda kv: section_sort_key(kv[0]))) or "none"),
        ("arg types", ", ".join(f"{k}={v}" for k, v in sorted(arg_types.items())) or "none"),
        ("seed", str(cli.seed)),
        ("cases", str(cli.cases)),
        ("laws/case", str(cli.laws_per_case)),
        ("depth", str(cli.depth)),
        ("coverage", "off" if cli.no_coverage else "on"),
        ("batches", "off" if cli.no_batches or cli.only_coverage else "on"),
        ("shrink", "on" if cli.shrink else "off"),
        ("fail-fast", str(cli.fail_fast)),
        ("rewritable '='", str(rewritable)),
        ("malformed", str(len(errors))),
    ]
    keyw = max(len(k) for k, _v in rows)
    for key, value in rows:
        print(f"  {key:<{keyw}}  {value}")


def print_inventory(specs: Sequence[PropertySpec], mode: str) -> None:
    if mode == "none":
        return
    print()
    print_rule("FUZZ INVENTORY")
    print()
    nested = [spec for spec in specs if spec.path.relative_to(PROPERTY_ROOT).as_posix().count("/") > 0]
    print(f"  loaded      {len(specs)} properties")
    print("  recursive   yes")
    print(f"  nested      {'yes' if nested else 'no'}" + (f" ({len(nested)} properties)" if nested else ""))
    if mode != "table":
        print("  listing     compact; use --inventory table for every READY row")
        return
    widths = widths_for(specs, len(specs))
    for i, spec in enumerate(specs, 1):
        print_inventory_status(spec, i, len(specs), widths)


def print_feature_summary(runner: FuzzRunner) -> None:
    if not runner.feature_stats:
        return
    print()
    print_rule("FEATURE COVERAGE")
    print()
    header = f"{'Feature':<32} {'Passed':>8} {'Failed':>8} {'Pass %':>8} {'Time':>14}"
    print(f"  {BOLD}{header}{RESET}")
    print(f"  {GRAY}{'-' * len(header)}{RESET}")
    for feature in sorted(runner.feature_stats):
        stats = runner.feature_stats[feature]
        ok = stats.passed + stats.xfailed
        bad = stats.failed + stats.xpassed
        print(f"  {feature:<32} {ok:>8}/{stats.total:<4} {bad:>8} {stats.percent:>7.1f}% {strip_ansi(format_duration(stats.elapsed_ns)):>14}")


def print_section_summary(runner: FuzzRunner) -> None:
    print_rule("SECTION SUMMARY")
    print()
    header = f"{'Section':<32} {'Passed':>8} {'Failed':>8} {'Pass %':>8} {'Time':>14}"
    print(f"  {BOLD}{header}{RESET}")
    print(f"  {GRAY}{'-' * len(header)}{RESET}")
    for section in sorted(runner.section_stats, key=section_sort_key):
        stats = runner.section_stats.get(section, SectionStats())
        _color, title = SECTION_STYLES.get(section, (YELLOW, section.title()))
        ok = stats.passed + stats.xfailed
        bad = stats.failed + stats.xpassed
        print(f"  {title:<32} {ok:>8}/{stats.total:<4} {bad:>8} {stats.percent:>7.1f}% {strip_ansi(format_duration(stats.elapsed_ns)):>14}")


def print_failure_clusters(failures: Sequence[CaseOutcome]) -> None:
    if not failures:
        return
    print()
    print_rule("FAILURE CLUSTERS")
    print()
    groups: dict[str, list[CaseOutcome]] = defaultdict(list)
    for failure in failures:
        groups[failure_signature(failure)].append(failure)
    for signature, items in sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0]))[:20]:
        first = items[0]
        print(f"  {len(items):4d}  {signature}")
        print(f"        first: {first.generated.spec.name}")
        if first.replay_path:
            try:
                replay = first.replay_path.relative_to(ROOT).as_posix()
            except ValueError:
                replay = first.replay_path.as_posix()
            print(f"        replay: {replay}")


def print_summary(stats: RunStats, runner: FuzzRunner, started_ns: int) -> None:
    print_rule("FUZZ SUMMARY")
    print()
    elapsed = time.perf_counter_ns() - started_ns
    rows = [
        ("Properties", f"{stats.selected:6d}"),
        ("Generated", f"{stats.generated:6d}"),
        ("Executed", f"{stats.executed_cases:6d}"),
        ("Passed", f"{stats.passed:6d}"),
        ("Failed", f"{stats.failed:6d}"),
        ("Blocked", f"{stats.blocked:6d}"),
        ("XFail", f"{stats.xfailed:6d}"),
        ("XPass", f"{stats.xpassed:6d}"),
        ("Rewrites", f"{stats.rewrites:6d}"),
        ("Shrinks", f"{stats.shrinks:6d}"),
        ("Compiled", f"{stats.compiled_programs:6d}"),
        ("Time", strip_ansi(format_duration(elapsed))),
    ]
    keyw = max(len(k) for k, _v in rows)
    for key, value in rows:
        print(f"  {key + ':':<{keyw + 1}} {value}")


def save_results(results: Sequence[CaseOutcome]) -> None:
    payload = {
        "timestamp": datetime.now().strftime("%Y-%m-%dT%H:%M:%S"),
        "results": {
            outcome.generated.spec.name: {
                "status": outcome.status,
                "passed": outcome.status in {"PASS", "XFAIL"},
                "message": outcome.message,
                "phase": outcome.phase,
                "seed": outcome.generated.seed,
            }
            for outcome in results
        },
    }
    RESULTS_FILE.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def save_first_failure(outcome: CaseOutcome | None) -> None:
    timestamp = datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
    if outcome is None:
        FIRST_FAILURE_FILE.write_text(
            "\n".join([
                "#+TITLE: Last First Fuzz Failure",
                f"#+DATE: {timestamp}",
                "",
                "[OBS id:obs.fuzz.last-first-failure src:tests/fuzzing/fuzz_codegen.py conf:high]",
                "  The latest fuzz run had no failing properties.",
                "",
            ]),
            encoding="utf-8",
        )
        return

    gen = outcome.generated
    replay = gen.spec.relpath
    if outcome.replay_path:
        try:
            replay = outcome.replay_path.relative_to(ROOT).as_posix()
        except ValueError:
            replay = outcome.replay_path.as_posix()
    lines = [
        "#+TITLE: Last First Fuzz Failure",
        f"#+DATE: {timestamp}",
        "",
        "[OBS id:obs.fuzz.last-first-failure src:tests/fuzzing/fuzz_codegen.py conf:high]",
        "  This record captures the first failing fuzz property from the latest run.",
        "",
        "* Failure",
        f"- Property: ={gen.spec.name}=",
        f"- Status: ={outcome.status}=",
        f"- Phase: ={outcome.phase}=",
        f"- Reason: ={outcome.message}=",
        f"- Source: ={gen.spec.relpath}:{gen.spec.line}=",
        f"- Replay: ={replay}=",
        f"- Expected: ={outcome.expected}=",
        f"- Actual: ={outcome.actual if outcome.actual is not None else '∅'}=",
        "",
        "** Generated Expression",
        "#+BEGIN_SRC monad",
        gen.source,
        "#+END_SRC",
    ]
    if gen.bindings:
        lines.extend(["", "** Bindings"])
        for name, expr in gen.bindings.items():
            lines.append(f"- ={name}=: ={expr.src}= ({expr.typ})")
    if outcome.compiler_output:
        lines.extend(["", "** Output", "#+BEGIN_EXAMPLE", strip_ansi(outcome.compiler_output).rstrip(), "#+END_EXAMPLE"])
    FIRST_FAILURE_FILE.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_json_summary(path: Path, stats: RunStats, runner: FuzzRunner, started_ns: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "stats": dataclasses.asdict(stats),
        "elapsed_ns": time.perf_counter_ns() - started_ns,
        "section_stats": {k: dataclasses.asdict(v) for k, v in runner.section_stats.items()},
        "feature_stats": {k: dataclasses.asdict(v) for k, v in runner.feature_stats.items()},
        "failures": [
            {
                "name": f.generated.spec.name,
                "section": f.generated.spec.section,
                "tier": f.generated.spec.tier,
                "kind": f.generated.spec.kind,
                "path": f.generated.spec.relpath,
                "line": f.generated.spec.line,
                "seed": f.generated.seed,
                "status": f.status,
                "phase": f.phase,
                "expected": f.expected,
                "actual": f.actual,
                "message": f.message,
                "replay": f.replay_path.as_posix() if f.replay_path else None,
                "expr": f.generated.source,
                "expr_before_rewrite": f.generated.source_before_rewrite,
                "rewrites": list(f.generated.rewrites),
                "signature": failure_signature(f),
            }
            for f in runner.failures
        ],
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")


### Run phases


def run_coverage(runner: FuzzRunner, cli: Cli, stats: RunStats) -> None:
    cases = coverage_cases(runner.specs, cli.seed, cli.depth)
    cases = sorted(cases, key=lambda c: (section_sort_key(section_key(c.spec)), c.ordinal))
    for i, case in enumerate(cases, 1):
        section = section_key(case.spec)
        runner.begin_section(section)
        stats.generated += 1
        stats.rewrites += len(case.rewrites)
        outcome = run_isolated_case(case, keep=cli.keep, label="coverage")
        stats.compiled_programs += 1
        stats.executed_cases += 1 if outcome.phase == "run" or case.spec.kind == "compile-fail" else 0
        if outcome.status == "FAIL" and cli.shrink:
            shrunk = try_shrink(outcome, max_steps=cli.max_shrink_steps)
            if shrunk.shrunk is not None:
                stats.shrinks += 1
            outcome = shrunk
        if outcome.status == "PASS":
            stats.passed += 1
        elif outcome.status == "XFAIL":
            stats.xfailed += 1
        elif outcome.status == "XPASS":
            stats.xpassed += 1
        else:
            stats.failed += 1
        show = outcome.status != "PASS" or cli.show_expr
        runner.record(outcome, i, show=show)
        if outcome.status == "PASS":
            runner.print_live_progress(case.spec, i, len(cases))
        if runner.failures and cli.fail_fast:
            break


def run_random_batches(runner: FuzzRunner, cli: Cli, stats: RunStats) -> None:
    if cli.only_coverage or cli.no_batches or cli.cases == 0:
        return
    eligible = [spec for spec in runner.specs if spec.kind == "law" and not spec.xfail]
    if not eligible:
        return
    runner.begin_section("batch")
    batch_widths = Widths(
        location=len(f"{Path(__file__).resolve()}:1:1: error:"),
        progress=len(progress_prefix(str(Path(__file__).resolve()), 1, 1, "error", f"{cli.cases:04d}/{cli.cases:04d}", "fuzz.batch.000000", colored=False, loc_width=len(f"{Path(__file__).resolve()}:1:1: error:"))),
    )
    for batch_index in range(1, cli.cases + 1):
        seed = cli.seed + batch_index - 1
        cases = random_batch(seed, cli.laws_per_case, cli.depth, eligible)
        if not cases:
            continue
        stats.generated += len(cases)
        stats.rewrites += sum(len(case.rewrites) for case in cases)
        result, _source, _expected, replay = compile_and_run(seed, cases, keep=cli.keep, label=f"batch_{batch_index:04d}")
        stats.compiled_programs += 1
        pseudo_spec = PropertySpec(
            name=f"fuzz.batch.{seed:06d}",
            section="batch",
            tier="stable",
            kind="law",
            args=(),
            typ="Bool",
            expect="True",
            expect_python=None,
            expect_diagnostic=None,
            description="random mixed law batch",
            law="True",
            program=None,
            features=("batch",),
            path=Path(__file__).resolve(),
            line=1,
        )
        pseudo_case = GeneratedCase(pseudo_spec, seed, batch_index, f"{len(cases)} mixed laws", f"{len(cases)} mixed laws", "ok", {}, (), 0)
        outcome = CaseOutcome(
            generated=pseudo_case,
            ok=result.ok,
            status="PASS" if result.ok else "FAIL",
            phase=result.phase,
            actual="ok" if result.ok else result.message,
            expected="ok",
            elapsed_ns=result.compile_ns + result.run_ns,
            replay_path=replay,
            message=result.message,
            compiler_output=result.output if not result.ok else "",
        )
        show_batch = not result.ok or batch_index == 1 or batch_index == cli.cases or batch_index % 10 == 0
        if show_batch:
            print_status(outcome, batch_index, cli.cases, batch_widths, show_expr=False)
        if result.ok:
            stats.executed_cases += len(cases)
            stats.passed += len(cases)
            continue

        stats.blocked += len(cases)
        print(f"    batch {batch_index:04d} failed in {result.phase}; isolating {len(cases)} law(s)")
        isolated = isolate_batch_failures(cases, keep=True, label=f"batch_{batch_index:04d}_isolated")
        for j, isolated_outcome in enumerate(isolated, 1):
            stats.compiled_programs += 1
            if isolated_outcome.ok:
                continue
            if isolated_outcome.status == "FAIL" and cli.shrink:
                shrunk = try_shrink(isolated_outcome, max_steps=cli.max_shrink_steps)
                if shrunk.shrunk is not None:
                    stats.shrinks += 1
                isolated_outcome = shrunk
            if isolated_outcome.status == "XFAIL":
                stats.xfailed += 1
            elif isolated_outcome.status == "XPASS":
                stats.xpassed += 1
                runner.failures.append(isolated_outcome)
            else:
                stats.failed += 1
                runner.failures.append(isolated_outcome)
            widths = widths_for([isolated_outcome.generated.spec], len(isolated))
            print_status(isolated_outcome, j, len(isolated), widths, show_expr=True)
        if runner.failures and cli.fail_fast:
            break


### CLI


def parse_args() -> Cli:
    parser = argparse.ArgumentParser(description="monadc recursive compiler fuzz runner")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--cases", type=int, default=96)
    parser.add_argument("--laws-per-case", "--forms", dest="laws_per_case", type=int, default=10)
    parser.add_argument("--depth", type=int, default=5)
    parser.add_argument("--properties", default="", help="comma-separated property names")
    parser.add_argument("--sections", default="", help="comma-separated raw or broad section names")
    parser.add_argument("--tiers", "--tier", dest="tiers", default="stable,compile-fail", help="comma-separated tiers or 'all'")
    parser.add_argument("--include-disabled", action="store_true", help="load .fuzz.disabled files too")
    parser.add_argument("--keep", action="store_true", help="keep replay files for passing programs too")
    parser.add_argument("--fail-fast", action="store_true", help="stop after first isolated failure")
    parser.add_argument("--only-coverage", action="store_true", help="run one isolated generated instance per selected property and stop")
    parser.add_argument("--no-coverage", action="store_true", help="skip isolated property coverage phase")
    parser.add_argument("--no-batches", action="store_true", help="skip random mixed batches")
    parser.add_argument("--inventory", choices=["none", "summary", "table"], default="summary")
    parser.add_argument("--list-properties", action="store_true", help="same as --inventory table with no execution")
    parser.add_argument("--show-expr", action="store_true", help="show generated expressions and bindings on passing rows too")
    parser.add_argument("--json-summary", type=Path)
    parser.add_argument("--width", type=int, default=WIDTH)
    parser.add_argument("--max-failures", type=int, default=25)
    parser.add_argument("--no-shrink", action="store_true", help="disable shrink-on-failure")
    parser.add_argument("--max-shrink-steps", type=int, default=20)
    parser.add_argument("--no-feature-summary", action="store_true")
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
    tiers = {item.strip() for item in args.tiers.split(",") if item.strip()}
    invalid_tiers = tiers - VALID_TIERS - {"all"}
    if invalid_tiers:
        raise SystemExit(f"unknown tier(s): {', '.join(sorted(invalid_tiers))}")
    inventory = "table" if args.list_properties else args.inventory
    return Cli(
        seed=args.seed,
        cases=args.cases,
        laws_per_case=args.laws_per_case,
        depth=args.depth,
        properties=properties,
        sections=sections,
        tiers=tiers,
        include_disabled=args.include_disabled,
        keep=args.keep,
        fail_fast=args.fail_fast,
        only_coverage=args.only_coverage,
        no_coverage=args.no_coverage,
        no_batches=args.no_batches,
        inventory=inventory,
        show_expr=args.show_expr,
        list_properties=args.list_properties,
        json_summary=args.json_summary,
        width=max(80, args.width),
        max_failures=args.max_failures,
        shrink=not args.no_shrink,
        max_shrink_steps=max(0, args.max_shrink_steps),
        feature_summary=not args.no_feature_summary,
    )


def clear_last_root() -> None:
    if not LAST_ROOT.exists():
        return
    for child in LAST_ROOT.glob("*_seed_*.mon"):
        stem = child.with_suffix("")
        for suffix in [".mon", ".expected", ".actual", ".compiler"]:
            target = stem.with_suffix(suffix)
            if target.exists():
                target.unlink()


def main() -> int:
    cli = parse_args()
    specs, errors = load_properties(include_disabled=cli.include_disabled)
    if errors:
        for err in errors:
            print(f"{Path(__file__).resolve()}:1:1: error: {err}", file=sys.stderr)
        raise SystemExit(2)
    selected = select_properties(specs, cli)
    if not selected:
        raise SystemExit("no fuzz properties selected")
    if not MONAD.exists() and not cli.list_properties:
        raise SystemExit(f"compiler not found at {MONAD}; run make first")

    print()
    print_box("RUNNING FUZZ SUITE")
    render_config(cli, specs, selected, errors)
    print_inventory(selected, cli.inventory)

    if cli.list_properties:
        return 0

    clear_last_root()
    stats = RunStats(
        loaded=len(specs),
        selected=len(selected),
        disabled=sum(1 for spec in specs if spec.disabled),
        malformed=len(errors),
    )
    runner = FuzzRunner(selected, cli)
    started_ns = time.perf_counter_ns()

    if not cli.no_coverage:
        run_coverage(runner, cli, stats)

    if not (runner.failures and cli.fail_fast):
        run_random_batches(runner, cli, stats)

    if runner.current_section is not None:
        runner.print_section_footer(runner.current_section)

    save_results(runner.results)
    save_first_failure(runner.first_failure)

    print()
    print_section_summary(runner)
    if cli.feature_summary:
        print_feature_summary(runner)
    print_failure_clusters(runner.failures[: cli.max_failures])
    print_summary(stats, runner, started_ns)

    if cli.json_summary:
        write_json_summary(cli.json_summary, stats, runner, started_ns)
        print(f"json summary: {cli.json_summary}")

    print()
    print_result_box(not runner.failures)
    return 1 if runner.failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
