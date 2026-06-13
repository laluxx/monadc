#!/usr/bin/env python3

from __future__ import annotations

import argparse
import dataclasses
import operator
import random
import re
import subprocess
import tempfile
import time
from datetime import datetime
from pathlib import Path
from typing import Sequence


ROOT = Path(__file__).resolve().parents[2]
FUZZ_ROOT = Path(__file__).resolve().parent
PROPERTY_ROOT = FUZZ_ROOT / "properties"
MONAD = ROOT / "monad"
WIDTH = 118

RESET = "\033[0m"
BOLD = "\033[1m"
BOLD_GREEN = "\033[1;32m"
BOLD_RED = "\033[1;31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
CYAN = "\033[36m"
MAGENTA = "\033[35m"
GRAY = "\033[90m"
WHITE = "\033[97m"


@dataclasses.dataclass(frozen=True)
class Expr:
    typ: str
    src: str
    value: object


@dataclasses.dataclass(frozen=True)
class PropertySpec:
    name: str
    section: str
    args: tuple[tuple[str, str], ...]
    typ: str
    expect: str
    description: str
    law: str
    path: Path
    line: int


@dataclasses.dataclass(frozen=True)
class GeneratedLaw:
    spec: PropertySpec
    seed: int
    ordinal: int
    src: str
    expected: str
    gen_elapsed_ns: int


@dataclasses.dataclass(frozen=True)
class CaseResult:
    seed: int
    passed: bool
    laws: list[GeneratedLaw]
    compile_elapsed_ns: int
    run_elapsed_ns: int
    stdout: str
    expected_stdout: str
    source: str
    source_path: Path | None
    message: str = ""
    compiler_output: str = ""
    shrink: str = ""


@dataclasses.dataclass(frozen=True)
class Cli:
    seed: int
    cases: int
    laws_per_case: int
    depth: int
    keep: bool
    properties: set[str]
    rich: bool
    show_expr: bool
    list_properties: bool


@dataclasses.dataclass
class RowWidths:
    location: int = 0
    progress: int = 0


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


def int_lit(rng: random.Random) -> Expr:
    value = rng.randint(-12, 12)
    return Expr("Int", str(value), value)


def gen_int(rng: random.Random, depth: int) -> Expr:
    if depth <= 0:
        return int_lit(rng)

    choice = rng.choice(["lit", "id", "add", "sub", "mul", "if", "with", "do"])
    if choice == "lit":
        return int_lit(rng)
    if choice == "id":
        expr = gen_int(rng, depth - 1)
        return Expr("Int", f"(fuzz-id-int {expr.src})", int(expr.value))
    if choice in {"add", "sub", "mul"}:
        left = gen_int(rng, depth - 1)
        right = gen_int(rng, depth - 1)
        op_src = {"add": "fuzz-add", "sub": "fuzz-sub", "mul": "fuzz-mul"}[choice]
        op_fn = {"add": operator.add, "sub": operator.sub, "mul": operator.mul}[choice]
        return Expr("Int", f"({op_src} {left.src} {right.src})", op_fn(int(left.value), int(right.value)))
    if choice == "if":
        cond = gen_bool(rng, depth - 1)
        then = gen_int(rng, depth - 1)
        other = gen_int(rng, depth - 1)
        value = then.value if cond.value else other.value
        return Expr("Int", f"(if {cond.src} {then.src} {other.src})", value)
    if choice == "with":
        bound = gen_int(rng, depth - 1)
        body = gen_int_with_var(rng, depth - 1, "x", int(bound.value))
        return Expr("Int", f"(with [x {bound.src}] {body.src})", body.value)

    first = gen_int(rng, depth - 1)
    second = gen_int(rng, depth - 1)
    return Expr("Int", f"(do {first.src} {second.src})", second.value)


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


def gen_bool(rng: random.Random, depth: int) -> Expr:
    left = gen_int(rng, max(depth - 1, 0))
    right = gen_int(rng, max(depth - 1, 0))
    op_src, op_fn = rng.choice([
        ("fuzz-eq-int?", operator.eq),
        ("!=", operator.ne),
        ("fuzz-lt?", operator.lt),
        ("fuzz-lte?", operator.le),
        ("fuzz-gt?", operator.gt),
        ("fuzz-gte?", operator.ge),
    ])
    return Expr("Bool", f"({op_src} {left.src} {right.src})", op_fn(int(left.value), int(right.value)))


def gen_arg(rng: random.Random, typ: str, depth: int) -> Expr:
    if typ == "Int":
        return gen_int(rng, depth)
    if typ == "Bool":
        return gen_bool(rng, depth)
    raise ValueError(f"unsupported fuzz argument type {typ!r}")


def load_properties() -> list[PropertySpec]:
    specs: list[PropertySpec] = []
    for path in sorted(PROPERTY_ROOT.glob("*.fuzz")):
        specs.append(load_property(path))
    if not specs:
        raise SystemExit(f"no fuzz property files found in {PROPERTY_ROOT}")
    return specs


def load_property(path: Path) -> PropertySpec:
    data: dict[str, str] = {}
    lines = path.read_text(encoding="utf-8").splitlines()
    line_numbers: dict[str, int] = {}
    for line_number, line in enumerate(lines, 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if ":" not in line:
            raise ValueError(f"{path}:{line_number}: expected 'key: value'")
        key, value = line.split(":", 1)
        key = key.strip()
        data[key] = value.strip()
        line_numbers.setdefault(key, line_number)

    required = ["name", "section", "args", "type", "expect", "description", "law"]
    missing = [key for key in required if key not in data]
    if missing:
        raise ValueError(f"{path}: missing keys: {', '.join(missing)}")

    args: list[tuple[str, str]] = []
    for item in data["args"].split():
        if ":" not in item:
            raise ValueError(f"{path}:{line_numbers['args']}: bad arg {item!r}; expected name:Type")
        name, typ = item.split(":", 1)
        if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
            raise ValueError(f"{path}:{line_numbers['args']}: bad arg name {name!r}")
        args.append((name, typ))

    return PropertySpec(
        name=data["name"],
        section=data["section"],
        args=tuple(args),
        typ=data["type"],
        expect=data["expect"],
        description=data["description"],
        law=data["law"],
        path=path,
        line=line_numbers["law"],
    )


def select_properties(specs: Sequence[PropertySpec], names: set[str]) -> list[PropertySpec]:
    if not names:
        return list(specs)
    by_name = {spec.name: spec for spec in specs}
    missing = sorted(names - set(by_name))
    if missing:
        raise SystemExit(f"unknown fuzz properties: {', '.join(missing)}")
    return [by_name[name] for name in sorted(names)]


def generate_law(spec: PropertySpec, rng: random.Random, seed: int, ordinal: int, depth: int) -> GeneratedLaw:
    start = time.perf_counter_ns()
    replacements: dict[str, str] = {}
    for name, typ in spec.args:
        replacements[name] = gen_arg(rng, typ, depth).src
    src = spec.law
    for name, value in replacements.items():
        src = src.replace("{" + name + "}", value)
    elapsed = time.perf_counter_ns() - start
    return GeneratedLaw(spec, seed, ordinal, src, spec.expect, elapsed)


def build_program(seed: int, laws_per_case: int, depth: int, specs: Sequence[PropertySpec]) -> tuple[str, str, list[GeneratedLaw]]:
    rng = random.Random(seed)
    laws = [
        generate_law(rng.choice(specs), rng, seed, ordinal, depth)
        for ordinal in range(1, laws_per_case + 1)
    ]
    lines = [
        ";; generated by tests/fuzzing/fuzz_codegen.py",
        f";; seed: {seed}",
        "(module Main)",
        "",
        STABLE_DEFS.rstrip(),
        "",
    ]
    expected: list[str] = []
    for index, law in enumerate(laws, start=1):
        rel = law.spec.path.relative_to(ROOT).as_posix()
        lines.append(f";; property {index}: {law.spec.name} from {rel}:{law.spec.line}")
        lines.append(f"(show {law.src})")
        expected.append(law.expected)
    return "\n".join(lines) + "\n", "\n".join(expected) + "\n", laws


def build_program_from_laws(seed: int, laws: Sequence[GeneratedLaw]) -> tuple[str, str]:
    lines = [
        ";; generated by tests/fuzzing/fuzz_codegen.py",
        f";; seed: {seed}",
        "(module Main)",
        "",
        STABLE_DEFS.rstrip(),
        "",
    ]
    expected: list[str] = []
    for index, law in enumerate(laws, start=1):
        rel = law.spec.path.relative_to(ROOT).as_posix()
        lines.append(f";; property {index}: {law.spec.name} from {rel}:{law.spec.line}")
        lines.append(f"(show {law.src})")
        expected.append(law.expected)
    return "\n".join(lines) + "\n", "\n".join(expected) + "\n"


def compile_and_run_source(seed: int, source: str, expected: str, prefix: str) -> tuple[bool, str, str, str]:
    with tempfile.TemporaryDirectory(prefix=prefix) as tmp_name:
        tmp = Path(tmp_name)
        src = tmp / f"fuzz_{seed}.mon"
        exe = tmp / f"fuzz_{seed}"
        src.write_text(source, encoding="utf-8")
        compile_result = subprocess.run(
            [str(MONAD), str(src), "-o", str(exe)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if compile_result.returncode != 0:
            return False, "compile failed", compile_result.stdout, ""
        run_result = subprocess.run(
            [str(exe)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if run_result.returncode != 0:
            return False, f"executable failed with exit code {run_result.returncode}", run_result.stdout, ""
        if run_result.stdout != expected:
            return False, "stdout mismatch", run_result.stdout, expected
        return True, "", run_result.stdout, expected


def shrink_failure(seed: int, laws: Sequence[GeneratedLaw]) -> str:
    for law in laws:
        source, expected = build_program_from_laws(seed, [law])
        passed, reason, _actual, _expected = compile_and_run_source(
            seed,
            source,
            expected,
            f"monadc-fuzz-shrink-{seed}-",
        )
        if not passed:
            return f"one-law counterexample: {law.spec.name} ({reason})"

    lo = 1
    hi = len(laws)
    best = len(laws)
    while lo <= hi:
        mid = (lo + hi) // 2
        source, expected = build_program_from_laws(seed, laws[:mid])
        passed, _reason, _actual, _expected = compile_and_run_source(
            seed,
            source,
            expected,
            f"monadc-fuzz-shrink-{seed}-",
        )
        if passed:
            lo = mid + 1
        else:
            best = mid
            hi = mid - 1
    return f"smallest failing prefix: {best} law(s)"


def run_case(seed: int, laws_per_case: int, depth: int, keep: bool, specs: Sequence[PropertySpec]) -> CaseResult:
    source, expected, laws = build_program(seed, laws_per_case, depth, specs)
    with tempfile.TemporaryDirectory(prefix=f"monadc-fuzz-{seed}-") as tmp_name:
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
        compile_elapsed = time.perf_counter_ns() - compile_start
        if compile_result.returncode != 0:
            shrink = shrink_failure(seed, laws)
            return CaseResult(
                seed=seed,
                passed=False,
                laws=laws,
                compile_elapsed_ns=compile_elapsed,
                run_elapsed_ns=0,
                stdout="",
                expected_stdout=expected,
                source=source,
                source_path=src,
                message="compile failed",
                compiler_output=compile_result.stdout,
                shrink=shrink,
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
        run_elapsed = time.perf_counter_ns() - run_start

        passed = run_result.returncode == 0 and run_result.stdout == expected
        message = ""
        if run_result.returncode != 0:
            message = f"executable failed with exit code {run_result.returncode}"
        elif run_result.stdout != expected:
            message = "stdout mismatch"

        kept_path: Path | None = None
        if keep or not passed:
            out_dir = FUZZ_ROOT / "last"
            out_dir.mkdir(parents=True, exist_ok=True)
            kept_path = out_dir / src.name
            kept_path.write_text(source, encoding="utf-8")
            (out_dir / f"{src.stem}.stdout").write_text(expected, encoding="utf-8")
            (out_dir / f"{src.stem}.actual").write_text(run_result.stdout, encoding="utf-8")

        shrink = "" if passed else shrink_failure(seed, laws)

        return CaseResult(
            seed=seed,
            passed=passed,
            laws=laws,
            compile_elapsed_ns=compile_elapsed,
            run_elapsed_ns=run_elapsed,
            stdout=run_result.stdout,
            expected_stdout=expected,
            source=source,
            source_path=kept_path,
            message=message,
            compiler_output=run_result.stdout,
            shrink=shrink,
        )


def parse_args() -> Cli:
    parser = argparse.ArgumentParser(description="QuickCheck-style type-directed codegen fuzzing.")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--cases", type=int, default=96)
    parser.add_argument("--laws-per-case", "--forms", dest="laws_per_case", type=int, default=10)
    parser.add_argument("--depth", type=int, default=4)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--properties", default="", help="comma-separated property names; default runs all property files")
    parser.add_argument("--list-properties", action="store_true")
    parser.add_argument("--no-rich", action="store_true", help="accepted for compatibility; output matches tests/run.py")
    parser.add_argument("--show-expr", action="store_true", help="show generated law expressions under each row")
    args = parser.parse_args()

    if args.cases < 1:
        raise SystemExit("--cases must be positive")
    if args.laws_per_case < 1:
        raise SystemExit("--laws-per-case must be positive")
    if args.depth < 0:
        raise SystemExit("--depth must be non-negative")

    names = {name.strip() for name in args.properties.split(",") if name.strip()}
    return Cli(
        seed=args.seed,
        cases=args.cases,
        laws_per_case=args.laws_per_case,
        depth=args.depth,
        keep=args.keep,
        properties=names,
        rich=not args.no_rich,
        show_expr=args.show_expr,
        list_properties=args.list_properties,
    )


def print_box(title: str) -> None:
    print(f"╔{'═' * WIDTH}╗")
    print(f"║{title.center(WIDTH)}║")
    print(f"╚{'═' * WIDTH}╝")


def print_section_line(title: str, color: str, *, bold: bool = False) -> None:
    text = f" {title} "
    width = WIDTH + 2
    left = max(6, (width - len(text)) // 3)
    right = max(1, width - left - len(text))
    weight = BOLD if bold else ""
    print(f"{color}{weight}{'─' * left}{text}{'─' * right}{RESET}")


def status_text(passed: bool) -> str:
    return f"{BOLD_GREEN}PASS{RESET}" if passed else f"{BOLD_RED}FAIL{RESET}"


def timestamp() -> str:
    return datetime.now().strftime("%H:%M:%S.%f")


def display_path(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path.resolve())


def location(spec: PropertySpec, severity: str) -> str:
    return f"{spec.path.resolve()}:{spec.line}:1: {severity}:"


def script_location(severity: str = "note") -> str:
    return f"{Path(__file__).resolve()}:1:1: {severity}:"


def format_duration(elapsed_ns: int) -> str:
    if elapsed_ns < 1_000:
        return color_duration(elapsed_ns, "ns")
    if elapsed_ns < 1_000_000:
        return color_duration(elapsed_ns / 1_000, "μs")
    if elapsed_ns < 1_000_000_000:
        return color_duration(elapsed_ns / 1_000_000, "ms")
    return color_duration(elapsed_ns / 1_000_000_000, "s")


def color_duration(value: float, unit: str) -> str:
    return f"[{value:7.2f} ] {CYAN}{unit}{RESET}"


def strip_ansi(value: str) -> str:
    return re.sub(r"\x1b\[[0-9;]*m", "", value)


def indent_output(output: str) -> str:
    return "\n".join(f"    {line}" for line in output.rstrip().splitlines())


def progress_prefix(loc: str, counter: str, name: str, *, colored: bool, loc_width: int) -> str:
    location_text = loc.ljust(loc_width)
    counter_text = f"{WHITE}{counter}{RESET}" if colored else counter
    return f"{location_text} [{counter_text}] {name}"


def compute_widths(specs: Sequence[PropertySpec], total_laws: int) -> RowWidths:
    locations = [location(spec, "note") for spec in specs]
    locations.extend(location(spec, "error") for spec in specs)
    locations.append(script_location("note"))
    loc_width = max(len(loc) for loc in locations)

    prefixes: list[str] = []
    for index, spec in enumerate(specs, 1):
        prefixes.append(progress_prefix(location(spec, "note"), f"{index:04d}/{len(specs):04d}", spec.name, colored=False, loc_width=loc_width))
    prefixes.append(progress_prefix(script_location(), "phase", "compile", colored=False, loc_width=loc_width))
    prefixes.append(progress_prefix(script_location(), "phase", "run", colored=False, loc_width=loc_width))
    for index, spec in enumerate(specs, 1):
        prefixes.append(progress_prefix(location(spec, "note"), f"{index:04d}/{total_laws:04d}", spec.name, colored=False, loc_width=loc_width))
    return RowWidths(location=loc_width, progress=max(len(prefix) for prefix in prefixes))


def print_aligned_row(loc: str, counter: str, name: str, passed: bool, elapsed_ns: int, widths: RowWidths, detail: str = "") -> None:
    prefix = progress_prefix(loc, counter, name, colored=True, loc_width=widths.location)
    plain_prefix = progress_prefix(loc, counter, name, colored=False, loc_width=widths.location)
    pad = " " * max(2, widths.progress - len(plain_prefix) + 2)
    suffix = f" {detail}" if detail else ""
    print(f"{prefix}{pad}{status_text(passed)} {format_duration(elapsed_ns)}{suffix}")


def print_property_inventory(specs: Sequence[PropertySpec], widths: RowWidths, detailed: bool = False) -> None:
    print_section_line("Fuzz Property Inventory", MAGENTA, bold=True)
    for index, spec in enumerate(specs, 1):
        args = " ".join(f"{name}:{typ}" for name, typ in spec.args)
        detail = f"{timestamp()} :: {args} -> {spec.typ} | expect {spec.expect}"
        print_aligned_row(location(spec, "note"), f"{index:04d}/{len(specs):04d}", spec.name, True, 0, widths, detail)
        if detailed:
            print(f"    section: {spec.section}")
            print(f"    law: {spec.law}")
            print(f"    why: {spec.description}")


def print_seed_header(seed: int, index: int, total: int, laws_per_case: int, depth: int) -> None:
    print_section_line(f"Seed {seed} ({index}/{total}) laws={laws_per_case} depth={depth}", CYAN, bold=True)


def print_phase(seed: int, phase: str, passed: bool, elapsed_ns: int, widths: RowWidths, detail: str = "") -> None:
    phase_detail = f"{timestamp()} seed={seed:06d}"
    if detail:
        phase_detail = f"{phase_detail} {detail}"
    print_aligned_row(script_location(), "phase", phase, passed, elapsed_ns, widths, phase_detail)


def print_law_row(law: GeneratedLaw, actual: str | None, passed: bool, index: int, total: int, widths: RowWidths, show_expr: bool) -> None:
    severity = "note" if passed else "error"
    expected = law.expected
    actual_text = actual if actual is not None else "<missing>"
    detail = f"{timestamp()} seed={law.seed:06d} type={law.spec.typ:<4} expect={expected:<5} actual={actual_text:<5}"
    print_aligned_row(location(law.spec, severity), f"{index:04d}/{total:04d}", law.spec.name, passed, law.gen_elapsed_ns, widths, detail)
    if show_expr:
        print(f"    expr: {law.src}")
        print(f"    from: {display_path(law.spec.path)}")
        print(f"    why: {law.spec.description}")


def print_failure(result: CaseResult) -> None:
    print(f"    reason: {result.message}")
    if result.shrink:
        print(f"    shrink: {result.shrink}")
    if result.source_path:
        print(f"    replay: {display_path(result.source_path)}")
    if result.compiler_output:
        print(indent_output(result.compiler_output))
    if result.stdout != result.expected_stdout:
        print("    expected stdout:")
        print(indent_output(result.expected_stdout))
        print("    actual stdout:")
        print(indent_output(result.stdout))


def render_summary(results: Sequence[CaseResult], total_laws: int, specs: Sequence[PropertySpec], started_ns: int) -> None:
    elapsed_ns = time.perf_counter_ns() - started_ns
    passed_cases = sum(1 for result in results if result.passed)
    failed_cases = len(results) - passed_cases
    print_section_line("Fuzz Summary", MAGENTA, bold=True)
    print(f"cases:      {passed_cases}/{len(results)} passed | {failed_cases} failed")
    print(f"laws:       {total_laws}/{total_laws} checked")
    print(f"properties: {len(specs)} files from {display_path(PROPERTY_ROOT)}")
    print(f"elapsed:    {format_duration(elapsed_ns)}")


def main() -> int:
    cli = parse_args()
    specs = select_properties(load_properties(), cli.properties)
    total_laws = cli.cases * cli.laws_per_case
    widths = compute_widths(specs, total_laws)

    if cli.list_properties:
        print_property_inventory(specs, widths, detailed=True)
        return 0

    if not MONAD.exists():
        raise SystemExit(f"compiler not found at {MONAD}; run make first")

    print_box("type-directed fuzzing")

    print_property_inventory(specs, widths, detailed=cli.show_expr)
    started_ns = time.perf_counter_ns()
    results: list[CaseResult] = []
    law_counter = 0

    for offset in range(cli.cases):
        seed = cli.seed + offset
        print_seed_header(seed, offset + 1, cli.cases, cli.laws_per_case, cli.depth)
        result = run_case(seed, cli.laws_per_case, cli.depth, cli.keep, specs)
        results.append(result)

        compile_ok = result.message != "compile failed"
        run_ok = compile_ok and not result.message.startswith("executable failed")
        print_phase(seed, "compile", compile_ok, result.compile_elapsed_ns, widths)
        print_phase(seed, "run", run_ok, result.run_elapsed_ns, widths)

        actual_lines = result.stdout.splitlines()
        for law_index, law in enumerate(result.laws):
            law_counter += 1
            actual = actual_lines[law_index] if law_index < len(actual_lines) else None
            law_passed = result.passed and actual == law.expected
            print_law_row(law, actual, law_passed, law_counter, total_laws, widths, cli.show_expr)

        if not result.passed:
            print_failure(result)
            render_summary(results, total_laws, specs, started_ns)
            return 1

    render_summary(results, total_laws, specs, started_ns)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
