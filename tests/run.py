#!/usr/bin/env python3
"""monadc test runner.

A verbose, Emacs-compilation-friendly test runner for the monadc repository.

Highlights:
  * no output truncation: failing compiler/runtime output is printed in full;
  * byte-safe subprocess capture: invalid UTF-8 cannot abort the suite;
  * recursive menu targets: `codegen errors reader`, `codegen types hm`, ...;
  * stable failure artifacts: tests/.last-failures/<test-id>/ survives cleanup;
  * duplicate TEST-ID/case-name detection before execution;
  * fail-fast, max-failures, only-failed, rerun-first-failure workflows;
  * richer JSON result records for dashboards and triage;
  * substring and regex diagnostic expectations.
"""

from __future__ import annotations

import argparse
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterable, Sequence


ROOT = Path(__file__).resolve().parent.parent
TEST_ROOT = ROOT / "tests"
CONTEXT_ROOT = ROOT / "context"
MONAD = ROOT / "monad"
RESULTS_FILE = TEST_ROOT / ".test-results.json"
FIRST_FAILURE_FILE = TEST_ROOT / ".last-first-failure.org"
FAILURE_DIR = TEST_ROOT / ".last-failures"
TRACKED_FILES: set[Path] | None = None
JSON_GOLDEN_TABLES: dict[Path, dict[str, object]] = {}
MONAD_TEST_ENV: dict[str, str] | None = None
WIDTH = 118

# ANSI styles. The runner intentionally keeps color because the output is meant
# to be read directly in terminals and Emacs compilation buffers.
RESET = "\033[0m"
WHITE = "\033[97m"
BOLD_GREEN = "\033[1;32m"
BOLD_RED = "\033[1;31m"
BOLD = "\033[1m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
GRAY = "\033[90m"
CYAN = "\033[36m"
BLUE = "\033[34m"
MAGENTA = "\033[35m"
BLACK_BOLD = "\033[1;30m"
BRACKET_COLOR = "\033[38;2;157;129;186m"

SECTION_STYLES: dict[str, tuple[str, str]] = {
    "interaction": (GREEN, "Feature Interactions"),
    "superset": (YELLOW, "Atom Supersets"),
    "cffi": (MAGENTA, "C FFI"),
    "refinement": (BLUE, "Refinement Types"),
    "wisp": (CYAN, "Wisp"),
    "layout": (MAGENTA, "Layouts"),
    "map": (CYAN, "Maps"),
    "ratio": (BLUE, "Ratios and Percentages"),
    "diagnostic": (YELLOW, "Compiler Diagnostics"),
    "codegen": (MAGENTA, "Runtime Codegen"),
    "codegen/branches": (MAGENTA, "Codegen Branch Menu"),
    "codegen/errors": (BOLD_RED, "Codegen Error Menu"),
    "codegen/types": (BLUE, "Type-System Menu"),
    "language": (BLUE, "Reader Language Atoms"),
    "sugar": (CYAN, "Reader Sugar Forms"),
    "misc": (YELLOW, "Miscellaneous"),
}

SECTION_ORDER: dict[str, int] = {
    "interaction": 0,
    "superset": 5,
    "cffi": 10,
    "refinement": 20,
    "wisp": 30,
    "layout": 40,
    "map": 50,
    "ratio": 60,
    "diagnostic": 65,
    "codegen": 70,
    "codegen/branches": 71,
    "codegen/errors": 72,
    "codegen/types": 73,
    "language": 80,
    "sugar": 90,
    "misc": 1000,
}

TIER_ORDER = {"regression": 0, "known-fail": 1, "future": 2, "generated": 3}
DEFAULT_TIERS = frozenset({"regression"})

IGNORED_DISCOVERY_DIRS = {
    ".git",
    "__pycache__",
    ".last-failures",
    ".monadc-test-artifacts",
    ".pytest_cache",
}


@dataclass(slots=True)
class TestCase:
    name: str
    metadata: dict[str, str]
    fixture: Path | None = None
    source: str | None = None
    origin: Path | None = None
    origin_line: int = 1
    origin_col: int = 1
    menu_path: tuple[str, ...] = field(default_factory=tuple)


@dataclass(slots=True)
class CommandResult:
    args: list[str]
    returncode: int
    stdout: str
    elapsed_ns: int


@dataclass(slots=True)
class TestResult:
    name: str
    passed: bool
    elapsed_ns: int
    message: str = ""
    output: str = ""
    section: str = "misc"
    menu_path: tuple[str, ...] = field(default_factory=tuple)
    fixture: str | None = None
    expectation: str = "parse-json"
    tier: str = "regression"
    location: str | None = None
    failure_kind: str | None = None
    artifact_dir: str | None = None


@dataclass(slots=True)
class SectionStats:
    total: int = 0
    passed: int = 0
    failed: int = 0
    elapsed_ns: int = 0

    @property
    def percent(self) -> float:
        return 100.0 if self.total == 0 else (self.passed / self.total) * 100.0


@dataclass(slots=True)
class SectionWidths:
    location: int = 0
    progress: int = 0


@dataclass(slots=True)
class RunnerOptions:
    filters: tuple[str, ...]
    fail_fast: bool
    max_failures: int | None
    keep_passing_artifacts: bool
    preserve_failures: bool
    no_color: bool


class Runner:
    def __init__(self, cases: list[TestCase], options: RunnerOptions) -> None:
        self.options = options
        self.results: list[TestResult] = []
        self.first_failure: tuple[TestCase, TestResult] | None = None
        self.total = len(cases)
        self.section_widths = section_widths(cases, self.total)
        self.current_section: str | None = None
        self.section_stats: dict[str, SectionStats] = {}
        for case in cases:
            section = section_key(case)
            self.section_stats.setdefault(section, SectionStats()).total += 1

    @property
    def failures(self) -> int:
        return sum(1 for result in self.results if not result.passed)

    @property
    def should_stop(self) -> bool:
        if self.options.fail_fast and self.failures > 0:
            return True
        if self.options.max_failures is not None and self.failures >= self.options.max_failures:
            return True
        return False

    def run(self, case: TestCase, suite_tmpdir: Path) -> None:
        section = section_key(case)
        if section != self.current_section:
            if self.current_section is not None:
                self.print_section_footer(self.current_section)
            self.current_section = section
            print_section_header(section)

        start = time.perf_counter_ns()
        safe = safe_name(case.name)
        test_tmpdir = suite_tmpdir / safe
        test_tmpdir.mkdir(parents=True, exist_ok=False)
        side_effects_before = artifact_snapshot(case.fixture) if case.fixture else set()
        passed = False
        message = ""
        output = ""
        artifact_dir: str | None = None
        failure_kind: str | None = None

        try:
            passed, message, output = run_case(case, test_tmpdir)
            if case.fixture:
                cleanup_fixture_artifacts(case.fixture, side_effects_before)
            leftovers = [] if not test_tmpdir.exists() else list(test_tmpdir.iterdir())
            if passed and leftovers and not self.options.keep_passing_artifacts:
                cleanup_artifacts(test_tmpdir)
                leftovers = list(test_tmpdir.iterdir()) if test_tmpdir.exists() else []
            if passed and leftovers and not self.options.keep_passing_artifacts:
                passed = False
                names = ", ".join(path.name for path in leftovers)
                message = f"test left temporary artifacts after cleanup: {names}"
                failure_kind = "artifact-leak"
        except Exception as exc:  # Keep the suite alive for runner bugs in one case.
            passed = False
            message = f"runner exception: {exc.__class__.__name__}: {exc}"
            output = ""
            failure_kind = "runner-exception"
        finally:
            if not passed and self.options.preserve_failures:
                artifact_dir = preserve_failure_artifacts(case, test_tmpdir, message, output)
            if passed and self.options.keep_passing_artifacts:
                artifact_dir = str(test_tmpdir)
            elif test_tmpdir.exists():
                shutil.rmtree(test_tmpdir, ignore_errors=True)

        elapsed_ns = time.perf_counter_ns() - start
        location = result_location_text(case, output, passed)
        if failure_kind is None and not passed:
            failure_kind = classify_failure(message, output)

        result = TestResult(
            name=case.name,
            passed=passed,
            elapsed_ns=elapsed_ns,
            message=message,
            output=output,
            section=section,
            menu_path=case.menu_path,
            fixture=display_path(case.fixture) if case.fixture else None,
            expectation=case.metadata.get("TEST-EXPECT", "parse-json"),
            tier=test_tier(case),
            location=location,
            failure_kind=failure_kind,
            artifact_dir=artifact_dir,
        )
        self.results.append(result)
        if not result.passed and self.first_failure is None:
            self.first_failure = (case, result)

        stats = self.section_stats.setdefault(section, SectionStats())
        if result.passed:
            stats.passed += 1
        else:
            stats.failed += 1
        stats.elapsed_ns += elapsed_ns

        widths = self.section_widths.get(section, SectionWidths())
        print_result(
            result,
            len(self.results),
            self.total,
            case,
            widths.progress,
            widths.location,
        )

    def print_section_footer(self, section: str) -> None:
        stats = self.section_stats.get(section, SectionStats())
        elapsed = format_duration(stats.elapsed_ns)
        color, title = section_style(section)
        summary = (
            f"{title}: {stats.passed}/{stats.total} passed ({stats.percent:5.1f}%) | "
            f"{stats.failed} failed | {strip_ansi(elapsed)}"
        )
        print_section_line(summary, color, bold=True)


# ---------------------------------------------------------------------------
# Discovery


def discover_tests() -> list[TestCase]:
    cases: list[TestCase] = []
    for fixture in sorted(TEST_ROOT.rglob("*.mon")):
        if should_skip_path(fixture):
            continue
        metadata = read_metadata(fixture)
        test_id = metadata.get("TEST-ID")
        if not test_id:
            continue
        name = test_id.removeprefix("tests.")
        case = TestCase(
            name=name,
            fixture=fixture,
            metadata=metadata,
            origin=fixture,
            origin_line=fixture_start_line(fixture),
            origin_col=1,
        )
        case.menu_path = derive_menu_path(case)
        cases.append(case)

    for corpus in discover_corpus_files():
        prefix = path_to_prefix(corpus.name)
        cases.extend(read_corpus(corpus, prefix))

    return sorted(
        cases,
        key=lambda case: (
            tier_sort_key(test_tier(case)),
            section_sort_key(section_key(case)),
            case.menu_path,
            case.name,
        ),
    )


def discover_corpus_files() -> list[Path]:
    """Return reader TSV corpora in canonical order.

    Historical snapshots sometimes carried reviewed corpora as
    ``*.fixed.tsv`` or ``*.updated.tsv``.  Prefer canonical names, but keep the
    aliases discoverable so reviewed corpora cannot silently fall out of the
    suite again.
    """
    canonical = [
        "reader-refinements.tsv",
        "reader-interactions.tsv",
        "reader-supersets.tsv",
        "reader-atoms.tsv",
        "reader-sugars.tsv",
    ]
    aliases = {
        "reader-atoms.tsv": ["reader-atoms.fixed.tsv"],
        "reader-interactions.tsv": ["reader-interactions.updated.tsv"],
    }
    found: list[Path] = []
    for name in canonical:
        path = TEST_ROOT / name
        if path.exists():
            found.append(path)
            continue
        for alias in aliases.get(name, []):
            alias_path = TEST_ROOT / alias
            if alias_path.exists():
                found.append(alias_path)
                break
    return found


def should_skip_path(path: Path) -> bool:
    rel_parts = path.relative_to(ROOT).parts if path.is_absolute() else path.parts
    return any(part in IGNORED_DISCOVERY_DIRS or part.startswith(".monadc-tests-") for part in rel_parts)


def path_to_prefix(name: str) -> str:
    table = {
        "reader-atoms.tsv": "language",
        "reader-atoms.fixed.tsv": "language",
        "reader-sugars.tsv": "sugar",
        "reader-refinements.tsv": "refinement",
        "reader-interactions.tsv": "interaction",
        "reader-interactions.updated.tsv": "interaction",
        "reader-supersets.tsv": "superset",
    }
    return table.get(name, "language")


def read_corpus(path: Path, prefix: str = "language") -> list[TestCase]:
    cases: list[TestCase] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) < 4:
            raise ValueError(f"{path}:{line_number}: expected at least 4 tab-separated fields")
        name, context, purpose, source = fields[:4]
        expected = fields[4].strip().replace("\\n", "\n") if len(fields) >= 5 else ""
        expected_diagnostic = fields[5].strip() if len(fields) >= 6 else ""
        test_id = f"tests.{prefix}.{name}"
        meta = {
            "TEST-ID": test_id,
            "TEST-CONTEXT": context,
            "TEST-PURPOSE": purpose,
            "TEST-EXPECT": "parse-json",
            "TEST-MENU": prefix,
            "TEST-MENU-PATH": prefix,
            "TEST-CLI-TARGET": prefix,
        }
        json_table = path.with_name(f"{path.stem}.golden.jsonl")
        if json_table.exists():
            meta["TEST-EXPECT-JSON-TABLE"] = json_table.resolve().relative_to(ROOT).as_posix()
            meta["TEST-EXPECT-JSON-KEY"] = name
        if expected:
            if expected.startswith("fail:"):
                meta["TEST-EXPECT"] = expected
                if expected_diagnostic:
                    meta["TEST-EXPECT-DIAGNOSTIC"] = expected_diagnostic
            else:
                meta["TEST-EXPECT-DESUGAR"] = expected
        if len(fields) >= 7 and fields[6].strip():
            meta["TEST-SUPERSET"] = fields[6].strip()
        if len(fields) >= 8 and fields[7].strip():
            meta["TEST-TIER"] = normalize_tier(fields[7].strip())
        else:
            meta["TEST-TIER"] = "regression"
        if len(fields) >= 9 and fields[8].strip():
            meta["TEST-STATUS"] = fields[8].strip()
        case = TestCase(
            name=f"{prefix}.{name}",
            metadata=meta,
            source=source.replace("\\n", "\n"),
            origin=path,
            origin_line=line_number,
            origin_col=1,
        )
        case.menu_path = derive_menu_path(case)
        cases.append(case)
    return cases


def read_metadata(path: Path) -> dict[str, str]:
    metadata: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith(";;"):
            if line.strip():
                break
            continue
        match = re.match(r";;\s*(TEST-[A-Z0-9-]+):\s*(.*)", line)
        if match:
            metadata[match.group(1)] = match.group(2).strip()
    return metadata


def fixture_start_line(path: Path) -> int:
    for line_number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith(";;"):
            continue
        return line_number
    return 1


def derive_menu_path(case: TestCase) -> tuple[str, ...]:
    metadata = case.metadata

    explicit_cli = metadata.get("TEST-CLI-TARGET", "").strip()
    if explicit_cli:
        return normalize_menu_tokens(explicit_cli)

    explicit_path = metadata.get("TEST-MENU-PATH", "").strip()
    if explicit_path:
        menu = normalize_menu_tokens(explicit_path)
        root = metadata.get("TEST-MENU", "").strip()
        if root:
            root_tokens = normalize_menu_tokens(root)
            if menu[: len(root_tokens)] != root_tokens:
                menu = root_tokens + menu
        return menu

    category = metadata.get("TEST-CATEGORY", "").strip()
    subset = metadata.get("TEST-SUBSET", "").strip()
    if category or subset:
        return normalize_menu_tokens("/".join(part for part in (category, subset) if part))

    if case.fixture:
        try:
            rel = case.fixture.resolve().relative_to(TEST_ROOT.resolve())
            parts = rel.parts
            if len(parts) >= 2 and parts[0] == "codegen":
                # codegen/<branches|errors|types>/.../<atom leaf>/<fixture.mon>
                return tuple(normalize_token(part) for part in parts[:-1])
            if len(parts) >= 2:
                return tuple(normalize_token(part) for part in parts[:-1])
        except ValueError:
            pass

    return (section_key(case.name),)


def normalize_menu_tokens(value: str | Sequence[str]) -> tuple[str, ...]:
    if isinstance(value, str):
        raw = re.split(r"[\s/\\:]+", value.strip())
    else:
        raw = list(value)
    return tuple(token for token in (normalize_token(part) for part in raw) if token)


def normalize_token(value: str) -> str:
    value = value.strip().lower()
    value = value.removeprefix("tests.")
    value = value.replace("_", "-")
    return re.sub(r"[^a-z0-9.+-]+", "-", value).strip("-")


# ---------------------------------------------------------------------------
# Tiers / metadata validation


def normalize_tier(value: str) -> str:
    value = value.strip().lower().replace("_", "-").replace(" ", "-")
    aliases = {
        "green": "regression",
        "pass": "regression",
        "passing": "regression",
        "stable": "regression",
        "xfail": "known-fail",
        "knownfail": "known-fail",
        "todo": "future",
        "aspirational": "future",
    }
    return aliases.get(value, value or "regression")


def test_tier(case: TestCase) -> str:
    return normalize_tier(case.metadata.get("TEST-TIER", "regression"))


def tier_sort_key(tier: str) -> tuple[int, str]:
    normalized = normalize_tier(tier)
    return (TIER_ORDER.get(normalized, 100), normalized)


def selected_tiers_from_args(args: argparse.Namespace) -> set[str] | None:
    if getattr(args, "all_tiers", False):
        return None
    raw_values = args.tier or ["regression"]
    selected: set[str] = set()
    for value in raw_values:
        for part in re.split(r"[,\s]+", value):
            if part.strip():
                selected.add(normalize_tier(part))
    return selected or set(DEFAULT_TIERS)


def format_tier_filter(selected: set[str] | None) -> str:
    if selected is None:
        return "all"
    return ",".join(sorted(selected, key=tier_sort_key))


def print_tier_summary(cases: list[TestCase]) -> None:
    counts: dict[str, int] = {}
    for case in cases:
        tier = test_tier(case)
        counts[tier] = counts.get(tier, 0) + 1
    print(f"{'Tier':<20} {'Tests':>8}")
    print(f"{'-' * 20} {'-' * 8}")
    for tier, count in sorted(counts.items(), key=lambda kv: tier_sort_key(kv[0])):
        print(f"{tier:<20} {count:>8}")


def split_context_refs(raw: str) -> list[str]:
    return [part.strip() for part in raw.split(',') if part.strip()]


def collect_context_ids() -> set[str]:
    ids: set[str] = set()
    if not CONTEXT_ROOT.exists():
        return ids
    pattern = re.compile(r'[:=]ID:\s+(monadc\.context\.\S+)')
    for org_file in sorted(CONTEXT_ROOT.glob('*.org')):
        for line in org_file.read_text(encoding='utf-8', errors='replace').splitlines():
            match = pattern.search(line)
            if match:
                ids.add(match.group(1))
    return ids


def validate_context_links(cases: list[TestCase]) -> list[str]:
    if not CONTEXT_ROOT.exists():
        return [f"{CONTEXT_ROOT}: error: context directory is missing; cannot validate TEST-CONTEXT links"]
    known = collect_context_ids()
    errors: list[str] = []
    for case in cases:
        contexts = split_context_refs(case.metadata.get('TEST-CONTEXT', ''))
        if not contexts:
            src = case.origin or case.fixture or TEST_ROOT
            errors.append(f"{src.resolve()}:{case.origin_line}:{case.origin_col}: error: {case.name} has no TEST-CONTEXT")
            continue
        for context_id in contexts:
            if context_id.startswith('monadc.context.') and context_id not in known:
                src = case.origin or case.fixture or TEST_ROOT
                errors.append(
                    f"{src.resolve()}:{case.origin_line}:{case.origin_col}: error: "
                    f"{case.name} references missing context ID {context_id}"
                )
    return errors


def validate_test_metadata(cases: list[TestCase]) -> list[str]:
    required = ("TEST-ID", "TEST-CONTEXT", "TEST-PURPOSE", "TEST-EXPECT", "TEST-TIER")
    errors: list[str] = []
    seen_ids: set[str] = set()
    for case in cases:
        src = case.origin or case.fixture or TEST_ROOT
        for key in required:
            if not case.metadata.get(key, "").strip():
                errors.append(f"{src.resolve()}:{case.origin_line}:{case.origin_col}: error: {case.name} missing {key}")
        test_id = case.metadata.get("TEST-ID", "")
        if test_id in seen_ids:
            errors.append(f"{src.resolve()}:{case.origin_line}:{case.origin_col}: error: duplicate TEST-ID {test_id}")
        seen_ids.add(test_id)
        tier = test_tier(case)
        if tier not in TIER_ORDER:
            errors.append(f"{src.resolve()}:{case.origin_line}:{case.origin_col}: error: {case.name} has unknown TEST-TIER {tier!r}")
    return errors


def print_validation_errors(title: str, errors: list[str]) -> None:
    if not errors:
        print_section_line(f"{title}: OK", GREEN, bold=True)
        return
    print_section_line(f"{title}: {len(errors)} problem(s)", BOLD_RED, bold=True)
    for error in errors:
        print(error)


# Filtering / menu


def filter_cases(cases: list[TestCase], args: argparse.Namespace) -> list[TestCase]:
    selected = cases
    selected_tiers = selected_tiers_from_args(args)
    if selected_tiers is not None:
        selected = [case for case in selected if test_tier(case) in selected_tiers]

    if args.only_failed:
        previous = load_previous_results(RESULTS_FILE)
        failed_names = {
            name for name, record in previous.items()
            if isinstance(record, dict) and not bool(record.get("passed", False))
        }
        selected = [case for case in selected if case.name in failed_names]

    if args.rerun_first_failure:
        first_name = load_first_failure_name(RESULTS_FILE)
        selected = [case for case in selected if case.name == first_name] if first_name else []

    filters = normalize_menu_tokens(args.filters)
    if filters:
        selected = [case for case in selected if case_matches_filters(case, filters)]

    if args.name:
        patterns = args.name
        selected = [case for case in selected if any(re.search(pat, case.name) for pat in patterns)]

    return selected


def case_matches_filters(case: TestCase, filters: tuple[str, ...]) -> bool:
    if not filters:
        return True
    haystacks = [case.menu_path, normalize_menu_tokens(case.name)]
    section = section_key(case)
    haystacks.append(normalize_menu_tokens(section))

    # Allow `errors reader` to match `codegen/errors/reader/...` and also
    # exact/prefix targets like `codegen types hm`.
    for hay in haystacks:
        if starts_with(hay, filters) or contains_contiguous(hay, filters):
            return True
    return False


def starts_with(hay: tuple[str, ...], needle: tuple[str, ...]) -> bool:
    return len(hay) >= len(needle) and hay[: len(needle)] == needle


def contains_contiguous(hay: tuple[str, ...], needle: tuple[str, ...]) -> bool:
    if len(needle) > len(hay):
        return False
    for i in range(0, len(hay) - len(needle) + 1):
        if hay[i : i + len(needle)] == needle:
            return True
    return False


def list_targets(cases: list[TestCase]) -> None:
    targets: dict[tuple[str, ...], int] = {}
    for case in cases:
        menu = case.menu_path or (section_key(case),)
        for depth in range(1, len(menu) + 1):
            targets[menu[:depth]] = targets.get(menu[:depth], 0) + 1
    for target, count in sorted(targets.items()):
        print(f"{' '.join(target):<72} {count:5d}")


def list_cases(cases: list[TestCase]) -> None:
    for case in cases:
        menu = "/".join(case.menu_path)
        print(f"{case.name:<72} {test_tier(case):<12} {menu}")


# ---------------------------------------------------------------------------
# Test execution


def run_case(case: TestCase, tmpdir: Path) -> tuple[bool, str, str]:
    expect = case.metadata.get("TEST-EXPECT", "parse-json")
    if expect == "inventory":
        return True, "", ""

    fixture = materialize_fixture(case, tmpdir)
    stem = case.fixture.stem if case.fixture else safe_name(case.name)
    output_base = tmpdir / stem
    stdout_path = case.fixture.with_suffix(".stdout") if case.fixture else None
    json_golden_path = case.fixture.with_suffix(".json") if case.fixture else None
    desugar_golden_path = case.fixture.with_suffix(".desugar") if case.fixture else None
    wants_json_or_desugar_golden = bool(
        (json_golden_path and json_golden_path.exists())
        or (desugar_golden_path and desugar_golden_path.exists())
    )

    if expect.startswith("fail:"):
        if wants_json_or_desugar_golden:
            emitted_result, _emitted, _emit_stdout = emit_and_check_reader_goldens(
                case, fixture, output_base, json_golden_path, desugar_golden_path
            )
            if emitted_result:
                return emitted_result

        exe = tmpdir / stem
        compile_flags = case.metadata.get("TEST-COMPILE-FLAGS", "").split()
        result = run_monad([str(fixture), *compile_flags, "-o", str(exe)])
        if result.returncode == 0:
            return False, f"expected {expect} but compile succeeded", result.stdout

        diag_problem = check_expected_diagnostics(case, result.stdout, stdout_path)
        if diag_problem:
            return False, diag_problem, result.stdout
        return True, "", result.stdout

    emitted_result, emitted, emit_stdout = emit_and_check_reader_goldens(
        case, fixture, output_base, json_golden_path, desugar_golden_path
    )
    if emitted_result:
        return emitted_result
    assert emitted is not None

    special = run_special_assertions(case, emitted)
    if special:
        return special

    expected_ir = case.metadata.get("TEST-EXPECT-IR-CONTAINS")
    if expected_ir:
        compile_flags = case.metadata.get("TEST-COMPILE-FLAGS", "").split()
        ir_result = run_monad([str(fixture), *compile_flags, "--emit-ir", "--emit-obj", "-o", str(output_base)])
        if ir_result.returncode != 0:
            return False, "--emit-ir failed", ir_result.stdout
        ir_path = fixture.with_suffix(".ll")
        try:
            ir_text = ir_path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            return False, f"could not read emitted IR: {error}", ir_result.stdout
        finally:
            try:
                ir_path.unlink()
            except OSError:
                pass
            try:
                fixture.with_suffix(".o").unlink()
            except OSError:
                pass
        if expected_ir not in ir_text:
            return False, f"IR did not contain {expected_ir!r}", ir_text

    expected_desugar = case.metadata.get("TEST-EXPECT-DESUGAR")
    if expected_desugar:
        actual = extract_desugared_ast(emit_stdout)
        if actual != expected_desugar:
            return False, f"desugared AST mismatch: expected {expected_desugar!r}, got {actual!r}", emit_stdout

    json_table_mismatch = compare_corpus_json_golden(case, emitted)
    if json_table_mismatch:
        return False, json_table_mismatch, emit_stdout

    should_compile = "compile" in expect or "run" in expect or bool(stdout_path and stdout_path.exists())
    if not should_compile:
        return True, "", emit_stdout

    exe = tmpdir / stem
    compile_flags = case.metadata.get("TEST-COMPILE-FLAGS", "").split()
    result = run_monad([str(fixture), *compile_flags, "-o", str(exe)])
    if result.returncode != 0:
        return False, "compile failed", result.stdout
    if not exe.is_file() or not (exe.stat().st_mode & 0o111):
        return False, f"expected executable at {exe}", result.stdout

    if "run" not in expect and not (stdout_path and stdout_path.exists()):
        return True, "", result.stdout

    run_result = run_command([str(exe)], cwd=ROOT)
    if run_result.returncode != 0:
        return False, f"executable failed with exit code {run_result.returncode}", run_result.stdout

    if stdout_path and stdout_path.exists():
        expected = stdout_path.read_text(encoding="utf-8", errors="replace")
        if not run_result.stdout.endswith(expected):
            return False, f"stdout did not end with {stdout_path.relative_to(ROOT)}", run_result.stdout

    return True, "", run_result.stdout


def materialize_fixture(case: TestCase, tmpdir: Path) -> Path:
    if case.fixture:
        return case.fixture
    if case.source is None:
        raise ValueError(f"{case.name}: missing fixture and source")
    path = tmpdir / f"{safe_name(case.name)}.mon"
    path.write_text(case.source.rstrip() + "\n", encoding="utf-8")
    return path


def run_monad(args: list[str]) -> CommandResult:
    return run_command([str(MONAD), *args], cwd=ROOT, env=MONAD_TEST_ENV)


def run_command(args: list[str], cwd: Path, env: dict[str, str] | None = None) -> CommandResult:
    start = time.perf_counter_ns()
    proc = subprocess.run(
        args,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    elapsed_ns = time.perf_counter_ns() - start
    stdout = proc.stdout.decode("utf-8", errors="replace")
    return CommandResult(args=args, returncode=proc.returncode, stdout=stdout, elapsed_ns=elapsed_ns)


def check_expected_diagnostics(case: TestCase, output: str, stdout_path: Path | None) -> str | None:
    normalized = normalize_golden_output(output)
    if stdout_path and stdout_path.exists():
        expected = stdout_path.read_text(encoding="utf-8", errors="replace")
        expected_norm = normalize_golden_output(expected)
        if expected_norm not in normalized:
            return f"diagnostic did not contain {stdout_path.relative_to(ROOT)}"

    expected_diagnostic = case.metadata.get("TEST-EXPECT-DIAGNOSTIC")
    if expected_diagnostic and expected_diagnostic not in output and expected_diagnostic not in normalized:
        return f"diagnostic did not contain {expected_diagnostic!r}"

    expected_regex = case.metadata.get("TEST-EXPECT-DIAGNOSTIC-REGEX")
    if expected_regex:
        try:
            if not re.search(expected_regex, output, flags=re.MULTILINE | re.DOTALL):
                return f"diagnostic did not match regex {expected_regex!r}"
        except re.error as exc:
            return f"invalid TEST-EXPECT-DIAGNOSTIC-REGEX {expected_regex!r}: {exc}"

    return None


def emit_and_check_reader_goldens(
    case: TestCase,
    fixture: Path,
    output_base: Path,
    json_golden_path: Path | None,
    desugar_golden_path: Path | None,
) -> tuple[tuple[bool, str, str] | None, object | None, str]:
    json_path = Path(f"{output_base}.json")
    emit_flags = case.metadata.get("TEST-COMPILE-FLAGS", "").split()
    wants_desugar_output = (
        bool(case.metadata.get("TEST-EXPECT-DESUGAR"))
        or bool(desugar_golden_path and desugar_golden_path.exists())
    )
    if wants_desugar_output and not any(
        flag.startswith("--trace=") for flag in emit_flags
    ):
        emit_flags.append("--trace=ast")
    result = run_monad([str(fixture), *emit_flags, "--emit-json", "-o", str(output_base)])
    if result.returncode != 0:
        return (False, "--emit-json failed", result.stdout), None, result.stdout
    if not json_path.is_file():
        return (False, f"expected JSON output at {json_path}", result.stdout), None, result.stdout

    try:
        emitted = json.loads(json_path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, json.JSONDecodeError) as error:
        return (False, f"could not read emitted JSON: {error}", result.stdout), None, result.stdout

    if json_golden_path and json_golden_path.exists():
        mismatch = compare_json_golden(json_golden_path, emitted)
        if mismatch:
            return (False, mismatch, result.stdout), None, result.stdout

    if desugar_golden_path and desugar_golden_path.exists():
        expected = desugar_golden_path.read_text(encoding="utf-8", errors="replace").strip()
        actual = extract_desugared_ast(result.stdout).strip()
        if actual != expected:
            diff = unified_diff_text(
                expected.splitlines(),
                actual.splitlines(),
                str(desugar_golden_path.relative_to(ROOT)),
                "actual desugared AST",
            )
            return (False, f"desugared AST did not match {desugar_golden_path.relative_to(ROOT)}\n{diff}", result.stdout), None, result.stdout

    return None, emitted, result.stdout


# ---------------------------------------------------------------------------
# Goldens / AST comparison


def compare_json_golden(path: Path, emitted: object) -> str | None:
    try:
        expected = json.loads(path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, json.JSONDecodeError) as error:
        return f"could not read expected AST JSON {path.relative_to(ROOT)}: {error}"

    expected_norm = normalize_ast_json(expected)
    emitted_norm = normalize_ast_json(emitted)
    if expected_norm == emitted_norm:
        return None

    expected_text = json.dumps(expected_norm, indent=2, sort_keys=True).splitlines()
    emitted_text = json.dumps(emitted_norm, indent=2, sort_keys=True).splitlines()
    diff = unified_diff_text(expected_text, emitted_text, str(path.relative_to(ROOT)), "actual AST JSON")
    return f"AST JSON did not match {path.relative_to(ROOT)}\n{diff}"


def compare_corpus_json_golden(case: TestCase, emitted: object) -> str | None:
    table_name = case.metadata.get("TEST-EXPECT-JSON-TABLE")
    if not table_name:
        return None
    table_path = (ROOT / table_name).resolve()
    table = load_json_golden_table(table_path)
    key = case.metadata.get("TEST-EXPECT-JSON-KEY", case.name.rsplit(".", 1)[-1])
    if key not in table:
        return None

    expected_norm = normalize_ast_json(table[key])
    emitted_norm = normalize_ast_json(emitted)
    if expected_norm == emitted_norm:
        return None

    expected_text = json.dumps(expected_norm, indent=2, sort_keys=True).splitlines()
    emitted_text = json.dumps(emitted_norm, indent=2, sort_keys=True).splitlines()
    diff = unified_diff_text(expected_text, emitted_text, f"{display_path(table_path)}:{key}", "actual AST JSON")
    return f"AST JSON did not match {display_path(table_path)} entry {key}\n{diff}"


def load_json_golden_table(path: Path) -> dict[str, object]:
    path = path.resolve()
    if path not in JSON_GOLDEN_TABLES:
        rows: dict[str, object] = {}
        for line_number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if not line or line.startswith("#"):
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: invalid JSONL golden row: {error}") from error
            name = row.get("name") if isinstance(row, dict) else None
            ast = row.get("json") if isinstance(row, dict) else None
            if not isinstance(name, str):
                raise ValueError(f"{path}:{line_number}: golden row needs string field 'name'")
            rows[name] = ast
        JSON_GOLDEN_TABLES[path] = rows
    return JSON_GOLDEN_TABLES[path]


def normalize_ast_json(value: object) -> object:
    if isinstance(value, dict):
        return {
            key: normalize_ast_json(child)
            for key, child in value.items()
            if key not in {"line", "col"}
        }
    if isinstance(value, list):
        return [normalize_ast_json(child) for child in value]
    return value


def unified_diff_text(expected: list[str], actual: list[str], expected_name: str, actual_name: str) -> str:
    return "\n".join(
        difflib.unified_diff(
            expected,
            actual,
            fromfile=expected_name,
            tofile=actual_name,
            lineterm="",
        )
    )


def extract_desugared_ast(output: str) -> str:
    lines = output.split("\n")
    in_block = False
    blocks: list[list[str]] = []
    parts: list[str] = []
    for line in lines:
        if line.startswith("=== desugared AST"):
            in_block = True
            parts = []
            continue
        if in_block:
            if line.startswith("=== end desugared AST"):
                blocks.append(parts)
                in_block = False
                continue
            if line.startswith("[compile]") or line.startswith("[opt]") or line.startswith("  wrote"):
                continue
            if line.strip():
                parts.append(line.strip())
    for block in reversed(blocks):
        if block:
            return "\n".join(block)
    return ""


def run_special_assertions(case: TestCase, emitted: object) -> tuple[bool, str, str] | None:
    test_id = case.metadata.get("TEST-ID", "")
    if test_id == "tests.reader.heap-literals":
        if not contains_object(emitted, {"type": "array", "is_heap": True}):
            return False, "JSON did not mark ~[1 2 3] as a heap array", ""
    return None


def contains_object(value: object, expected: dict[str, object]) -> bool:
    if isinstance(value, dict):
        if all(value.get(key) == expected_value for key, expected_value in expected.items()):
            return True
        return any(contains_object(child, expected) for child in value.values())
    if isinstance(value, list):
        return any(contains_object(child, expected) for child in value)
    return False


# ---------------------------------------------------------------------------
# Artifacts / cleanup


def artifact_snapshot(fixture: Path) -> set[Path]:
    return {path.resolve() for path in artifact_candidates(fixture) if path.exists()}


def cleanup_fixture_artifacts(fixture: Path, before: set[Path]) -> None:
    for path in artifact_candidates(fixture):
        resolved = path.resolve()
        if path.exists() and resolved not in before and resolved not in tracked_files():
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()


def artifact_candidates(fixture: Path) -> list[Path]:
    stem = fixture.with_suffix("")
    return [
        stem.with_suffix(".o"),
        stem.with_suffix(".ll"),
        stem.with_suffix(".bc"),
        stem.with_suffix(".s"),
        stem,
        Path(f"{stem}_test"),
    ]


def preserve_failure_artifacts(case: TestCase, tmpdir: Path, message: str, output: str) -> str:
    FAILURE_DIR.mkdir(parents=True, exist_ok=True)
    dest = FAILURE_DIR / safe_name(case.name)
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)

    if tmpdir.exists():
        for child in tmpdir.iterdir():
            target = dest / child.name
            if child.is_dir():
                shutil.copytree(child, target)
            else:
                shutil.copy2(child, target)

    if case.fixture and case.fixture.exists():
        shutil.copy2(case.fixture, dest / case.fixture.name)
    elif case.source is not None:
        (dest / f"{safe_name(case.name)}.mon").write_text(case.source.rstrip() + "\n", encoding="utf-8")

    (dest / "output.log").write_text(output, encoding="utf-8", errors="replace")
    (dest / "metadata.json").write_text(
        json.dumps(
            {
                "name": case.name,
                "message": message,
                "origin": display_path(case.origin) if case.origin else None,
                "origin_line": case.origin_line,
                "origin_col": case.origin_col,
                "fixture": display_path(case.fixture) if case.fixture else None,
                "menu_path": list(case.menu_path),
                "metadata": case.metadata,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    return str(dest)


def cleanup_artifacts(path: Path) -> None:
    for child in path.iterdir():
        if child.is_dir():
            shutil.rmtree(child)
        else:
            child.unlink()


def cleanup_suite_artifacts() -> None:
    if RESULTS_FILE.exists() and RESULTS_FILE.resolve() not in tracked_files():
        # We keep RESULTS_FILE because it powers --only-failed and regressions.
        pass
    pycache = TEST_ROOT / "__pycache__"
    if pycache.exists() and pycache.is_dir():
        for child in pycache.iterdir():
            if child.resolve() in tracked_files():
                continue
            if child.is_dir():
                shutil.rmtree(child)
            else:
                child.unlink()
        if not any(pycache.iterdir()) and pycache.resolve() not in tracked_files():
            pycache.rmdir()
    for fixture in TEST_ROOT.rglob("*.mon"):
        if not should_skip_path(fixture):
            cleanup_fixture_artifacts(fixture, set())


def tracked_files() -> set[Path]:
    global TRACKED_FILES
    if TRACKED_FILES is None:
        result = run_command(["git", "ls-files", "tests"], cwd=ROOT)
        TRACKED_FILES = {
            (ROOT / line).resolve()
            for line in result.stdout.splitlines()
            if line.strip()
        }
    return TRACKED_FILES


# ---------------------------------------------------------------------------
# Result persistence / classification


def load_previous_results(path: Path) -> dict[str, object]:
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
        results = data.get("results", {}) if isinstance(data, dict) else {}
        return results if isinstance(results, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def load_first_failure_name(path: Path) -> str | None:
    if not path.exists():
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, json.JSONDecodeError):
        return None
    first = data.get("first_failure") if isinstance(data, dict) else None
    return first if isinstance(first, str) else None


def save_results(path: Path, results: list[TestResult], first_failure: tuple[TestCase, TestResult] | None) -> None:
    first_name = first_failure[1].name if first_failure else None
    data = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "root": str(ROOT),
        "first_failure": first_name,
        "summary": {
            "total": len(results),
            "passed": sum(1 for r in results if r.passed),
            "failed": sum(1 for r in results if not r.passed),
        },
        "results": {
            r.name: {
                "passed": r.passed,
                "message": r.message,
                "section": r.section,
                "menu_path": list(r.menu_path),
                "fixture": r.fixture,
                "expectation": r.expectation,
                "tier": r.tier,
                "elapsed_ns": r.elapsed_ns,
                "location": r.location,
                "failure_kind": r.failure_kind,
                "artifact_dir": r.artifact_dir,
            }
            for r in results
        },
    }
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def save_first_failure(path: Path, first_failure: tuple[TestCase, TestResult] | None) -> None:
    timestamp = time.strftime("%Y-%m-%dT%H:%M:%S")
    if first_failure is None:
        path.write_text(
            "\n".join(
                [
                    "#+TITLE: Last First Failing Test",
                    f"#+DATE: {timestamp}",
                    "",
                    "[OBS id:obs.tests.last-first-failure src:tests/run.py conf:high]",
                    "  The latest test-suite run had no failing tests.",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        return

    case, result = first_failure
    fixture = case.fixture.relative_to(ROOT) if case.fixture else None
    source = case.source if case.source is not None else ""
    output = normalize_record_output(result.output)
    lines = [
        "#+TITLE: Last First Failing Test",
        f"#+DATE: {timestamp}",
        "",
        "[OBS id:obs.tests.last-first-failure src:tests/run.py conf:high]",
        "  This generated record captures the first failing test from the latest",
        "  suite run. It is intentionally overwritten on each run so the next",
        "  debugging pass can start from the earliest observed failure.",
        "",
        "* Failure",
        f"- Test: ={case.name}=",
        f"- Status: =FAIL=",
        f"- Menu: ={'/'.join(case.menu_path)}=",
        f"- Duration: ={strip_ansi(format_duration(result.elapsed_ns))}=",
        f"- Reason: ={result.message or 'failed'}=",
        f"- Failure kind: ={result.failure_kind or 'unknown'}=",
    ]
    if fixture:
        lines.append(f"- Fixture: ={fixture}=")
    if result.artifact_dir:
        lines.append(f"- Artifacts: ={display_path(Path(result.artifact_dir))}=")
    if source:
        lines.extend(["", "** Materialized Source", "#+BEGIN_SRC monad", source.rstrip(), "#+END_SRC"])

    lines.extend(["", "** Metadata"])
    for key in sorted(case.metadata):
        lines.append(f"- ={key}=: {case.metadata[key]}")

    contexts = case.metadata.get("TEST-CONTEXT", "").strip()
    if contexts:
        lines.extend(["", "** Context Links"])
        for context_id in [part.strip() for part in contexts.split(",") if part.strip()]:
            lines.append(f"- ={context_id}=")

    if output:
        lines.extend(["", "** Output", "#+BEGIN_EXAMPLE", output.rstrip(), "#+END_EXAMPLE"])

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def normalize_record_output(output: str) -> str:
    return strip_ansi(normalize_golden_output(output))


def classify_failure(message: str, output: str) -> str:
    text = f"{message}\n{output}"
    if "expected fail:compile but compile succeeded" in text:
        return "expected-fail-compiled"
    if "diagnostic did not contain" in text or "diagnostic did not match regex" in text:
        return "diagnostic-mismatch"
    if "AST JSON did not match" in text:
        return "json-golden-mismatch"
    if "desugared AST" in text:
        return "desugar-mismatch"
    if "stdout" in text:
        return "stdout-mismatch"
    if "--emit-json failed" in text:
        return "emit-json-failed"
    if "compile failed" in text:
        return "compile-failed"
    if "segmentation fault" in text.lower() or "signal 11" in text.lower():
        return "segfault"
    return "failure"


def compute_changes(previous: dict[str, object], results: list[TestResult]) -> tuple[list[str], list[str]]:
    current = {r.name: r.passed for r in results}
    previous_bool = {
        name: bool(record.get("passed", False))
        for name, record in previous.items()
        if isinstance(record, dict)
    }
    regressions = [name for name, passed in current.items() if not passed and previous_bool.get(name, True)]
    fixes = [name for name, passed in current.items() if passed and name in previous_bool and not previous_bool[name]]
    return sorted(regressions), sorted(fixes)


# ---------------------------------------------------------------------------
# Duplicate validation


def find_duplicate_cases(cases: list[TestCase]) -> dict[str, list[TestCase]]:
    seen: dict[str, list[TestCase]] = {}
    for case in cases:
        seen.setdefault(case.name, []).append(case)
    return {name: group for name, group in seen.items() if len(group) > 1}


def print_duplicate_cases(duplicates: dict[str, list[TestCase]]) -> None:
    for name, group in sorted(duplicates.items()):
        first = group[0]
        origin = first.origin or first.fixture or TEST_ROOT
        print(f"{origin.resolve()}:{first.origin_line}:{first.origin_col}: error: duplicate test case name '{name}'")
        for case in group:
            src = case.origin or case.fixture or TEST_ROOT
            print(f"    duplicate: {src.resolve()}:{case.origin_line}:{case.origin_col} menu={'/'.join(case.menu_path)}")


# ---------------------------------------------------------------------------
# Presentation


def print_box(title: str) -> None:
    print(f"╔{'═' * WIDTH}╗")
    print(f"║{title.center(WIDTH)}║")
    print(f"╚{'═' * WIDTH}╝")


def print_result_box(passed: bool) -> None:
    color = BOLD_GREEN if passed else BOLD_RED
    label = "TESTS PASSED" if passed else "TESTS FAILED"
    inner = WIDTH
    print(f"{color}╔{'═' * inner}╗{RESET}")
    print(f"{color}║{label.center(inner)}║{RESET}")
    print(f"{color}╚{'═' * inner}╝{RESET}")


def section_style(section: str) -> tuple[str, str]:
    if section in SECTION_STYLES:
        return SECTION_STYLES[section]
    if section.startswith("codegen/errors"):
        return BOLD_RED, title_from_section(section)
    if section.startswith("codegen/types"):
        return BLUE, title_from_section(section)
    if section.startswith("codegen/branches"):
        return MAGENTA, title_from_section(section)
    return YELLOW, title_from_section(section)


def title_from_section(section: str) -> str:
    return " ".join(part.capitalize() for part in section.replace("-", " ").split("/"))


def print_section_header(section: str) -> None:
    color, title = section_style(section)
    print()
    heading = f"{title} ({section})"
    print_section_line(heading, color, bold=True)


def background_code(color: str) -> str:
    if not color:
        return ""
    match = re.search(r"\[(?:1;)?3(\d)m", color)
    digit = match.group(1) if match else "7"
    return f"\033[4{digit}m"


def print_section_line(title: str, color: str, *, bold: bool = False) -> None:
    _ = bold
    bg = background_code(color)
    label = f" {title} "
    cap_right = f"{color}{chr(0x25d7)}{RESET}"
    print(f"{bg}{BLACK_BOLD}{label}{RESET}{cap_right}")


def print_result(
    result: TestResult,
    index: int,
    total: int,
    case: TestCase,
    prefix_width: int,
    loc_width: int,
) -> None:
    status = f"{BOLD_GREEN}PASS{RESET}" if result.passed else f"{BOLD_RED}FAIL{RESET}"
    timing = format_duration(result.elapsed_ns)
    loc_path, loc_line, loc_col, severity = compilation_location(case, result)
    counter = f"{index:04d}/{total:04d}"
    prefix = progress_prefix(loc_path, loc_line, loc_col, severity, counter, result.name, colored=True, loc_width=loc_width)
    plain_prefix = progress_prefix(loc_path, loc_line, loc_col, severity, counter, result.name, colored=False, loc_width=loc_width)
    pad = " " * max(2, prefix_width - len(plain_prefix) + 2)
    print(f"{prefix}{pad}{status} {timing}")
    if not result.passed:
        if result.message:
            print(f"    reason: {result.message}")
        if result.artifact_dir:
            print(f"    artifacts: {display_path(Path(result.artifact_dir))}")
        if result.output:
            # Intentionally no truncation.
            print(indent_output(result.output))


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


def location_prefix_width(cases: list[TestCase]) -> int:
    width = 0
    for case in cases:
        origin = case.origin or case.fixture or TEST_ROOT
        loc_path = str(origin.resolve())
        for severity in ("note", "error"):
            width = max(width, len(f"{loc_path}:{case.origin_line}:{case.origin_col}: {severity}:"))
    return width


def progress_prefix_width(cases: list[TestCase], loc_width: int, total: int | None = None) -> int:
    total = total or len(cases)
    width = 0
    for index, case in enumerate(cases, 1):
        origin = case.origin or case.fixture or TEST_ROOT
        loc_path = str(origin.resolve())
        counter = f"{index:04d}/{total:04d}"
        for severity in ("note", "error"):
            width = max(
                width,
                len(progress_prefix(loc_path, case.origin_line, case.origin_col, severity, counter, case.name, colored=False, loc_width=loc_width)),
            )
    return width


def section_widths(cases: list[TestCase], total: int) -> dict[str, SectionWidths]:
    grouped: dict[str, list[TestCase]] = {}
    for case in cases:
        grouped.setdefault(section_key(case), []).append(case)

    widths: dict[str, SectionWidths] = {}
    for section, section_cases in grouped.items():
        loc_width = location_prefix_width(section_cases)
        widths[section] = SectionWidths(location=loc_width, progress=progress_prefix_width(section_cases, loc_width, total))
    return widths


def compilation_location(case: TestCase, result: TestResult) -> tuple[str, int, int, str]:
    if not result.passed:
        found = extract_compiler_location(result.output)
        if found:
            return found
    origin = case.origin or case.fixture or TEST_ROOT
    return str(origin.resolve()), case.origin_line, case.origin_col, "note" if result.passed else "error"


def result_location_text(case: TestCase, output: str, passed: bool) -> str:
    result = TestResult(case.name, passed, 0, output=output)
    loc = compilation_location(case, result)
    return f"{loc[0]}:{loc[1]}:{loc[2]}:{loc[3]}"


def extract_compiler_location(output: str) -> tuple[str, int, int, str] | None:
    pattern = re.compile(r"(?m)^([^:\n]+):(\d+):(\d+):\s*(error|warning|note):")
    match = pattern.search(output)
    if not match:
        return None
    path_text, line, col, severity = match.groups()
    path = Path(path_text)
    if not path.is_absolute():
        path = (ROOT / path).resolve()
    return str(path), int(line), int(col), severity


def print_section_summary(runner: Runner) -> None:
    print_rule("SECTION SUMMARY")
    print()
    header = f"{'Section':<32} {'Passed':>8} {'Failed':>8} {'Pass %':>8} {'Time':>14}"
    print(f"  {BOLD}{header}{RESET}")
    print(f"  {GRAY}{'-' * len(header)}{RESET}")
    for section in sorted(runner.section_stats, key=section_sort_key):
        stats = runner.section_stats.get(section, SectionStats())
        _color, title = section_style(section)
        print(
            f"  {title:<32} {stats.passed:>8}/{stats.total:<4} {stats.failed:>8} "
            f"{stats.percent:>7.1f}% {strip_ansi(format_duration(stats.elapsed_ns)):>14}"
        )


def print_failure_kind_summary(results: list[TestResult]) -> None:
    failures = [r for r in results if not r.passed]
    if not failures:
        return
    buckets: dict[str, int] = {}
    for result in failures:
        key = result.failure_kind or "failure"
        buckets[key] = buckets.get(key, 0) + 1
    print_rule("FAILURE KIND SUMMARY")
    print()
    for key, count in sorted(buckets.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f"  {key:<36} {count:>6}")


def print_changes(regressions: list[str], fixes: list[str]) -> None:
    if not regressions and not fixes:
        return
    print()
    if regressions:
        print_section_line(f"REGRESSIONS (pass {chr(8594)} fail)", BOLD_RED)
        print_name_grid(regressions, "x", BOLD_RED)
    if fixes:
        print_section_line(f"FIXES (fail {chr(8594)} pass)", GREEN)
        print_name_grid(fixes, "+", BOLD_GREEN)


def print_name_grid(names: list[str], marker: str, marker_color: str, columns: int = 3) -> None:
    if not names:
        return
    rows = (len(names) + columns - 1) // columns
    col_widths = [0] * columns
    for index, name in enumerate(names):
        col = index // rows
        col_widths[col] = max(col_widths[col], len(name))
    for row in range(rows):
        cells = []
        for col in range(columns):
            index = col * rows + row
            if index < len(names):
                name = names[index]
                padded = name.ljust(col_widths[col])
                cells.append(f"{marker_color}{marker}{RESET} {padded}")
        print(f"    {'  '.join(cells).rstrip()}")


def print_rule(title: str) -> None:
    print_section_line(title, WHITE, bold=True)


def section_key(case: TestCase | str) -> str:
    if isinstance(case, str):
        name = case
        metadata: dict[str, str] = {}
        fixture_name = ""
        menu: tuple[str, ...] = ()
    else:
        name = case.name
        metadata = case.metadata
        fixture_name = case.fixture.name if case.fixture else ""
        menu = case.menu_path

    explicit = metadata.get("TEST-SECTION", "").strip().lower()
    if explicit:
        return explicit

    if len(menu) >= 2 and menu[0] == "codegen" and menu[1] in {"branches", "errors", "types"}:
        return f"codegen/{menu[1]}"

    if name.startswith("interaction.") or "interaction" in name or "interaction" in fixture_name:
        return "interaction"
    if name.startswith("superset."):
        return "superset"
    if name.startswith("cffi."):
        return "cffi"
    if name.startswith("refinement."):
        return "refinement"
    if name.startswith("wisp.") or ".wisp" in name or fixture_name.startswith("wisp_") or "rt_wisp" in fixture_name:
        return "wisp"
    if "layout" in name or "layout" in fixture_name:
        return "layout"
    if ".rt-map" in name or "map" in fixture_name or "-map-" in name:
        return "map"
    if "percent" in name or "ratio" in name or "periodic" in name:
        return "ratio"

    context = metadata.get("TEST-CONTEXT", "")
    if "interaction" in context:
        return "interaction"
    if "superset" in context:
        return "superset"
    if "cffi" in context or "ffi" in context:
        return "cffi"
    if "refinement" in context:
        return "refinement"
    if "wisp" in context:
        return "wisp"

    return name.split(".", 1)[0] if "." in name else "misc"


def section_sort_key(section: str) -> tuple[int, str]:
    return (SECTION_ORDER.get(section, 1000), section)


def format_duration(elapsed_ns: int) -> str:
    if elapsed_ns < 1_000:
        return color_duration(elapsed_ns, "ns")
    if elapsed_ns < 1_000_000:
        return color_duration(elapsed_ns / 1_000, "μs")
    if elapsed_ns < 1_000_000_000:
        return color_duration(elapsed_ns / 1_000_000, "ms")
    return color_duration(elapsed_ns / 1_000_000_000, "s")


def color_duration(value: float, unit: str) -> str:
    return f"{BRACKET_COLOR}[{RESET}{value:7.2f} {BRACKET_COLOR}]{RESET} {CYAN}{unit:>2}{RESET}"


def strip_ansi(value: str) -> str:
    return re.sub(r"\x1b\[[0-9;]*m", "", value)


def indent_output(output: str) -> str:
    return "\n".join(f"    {line}" for line in output.rstrip().splitlines())


def normalize_golden_output(output: str) -> str:
    return output.replace(str(ROOT), "<repo>")


def display_path(path: Path | None) -> str:
    if path is None:
        return ""
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name)


def disable_color() -> None:
    color_names = [
        "RESET", "WHITE", "BOLD_GREEN", "BOLD_RED", "BOLD", "GREEN",
        "YELLOW", "GRAY", "CYAN", "BLUE", "MAGENTA", "BLACK_BOLD",
        "BRACKET_COLOR",
    ]
    for name in color_names:
        globals()[name] = ""


# ---------------------------------------------------------------------------
# CLI


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="tests/run.py",
        description="Run monadc tests with verbose output and recursive menu filtering.",
    )
    parser.add_argument("filters", nargs="*", help="recursive menu filters, e.g. codegen errors reader, errors env, types hm")
    parser.add_argument("--list-targets", action="store_true", help="list recursive CLI/menu targets and exit")
    parser.add_argument("--list", action="store_true", help="list selected tests and exit")
    parser.add_argument("--list-tiers", action="store_true", help="list test tiers and exit")
    parser.add_argument("--tier", action="append", default=None, help="test tier(s) to run: regression, known-fail, future, generated; comma-separated values are allowed")
    parser.add_argument("--all-tiers", action="store_true", help="run/list every tier instead of the default regression gate")
    parser.add_argument("--validate-context-links", action="store_true", help="validate every TEST-CONTEXT value against context/*.org IDs and exit")
    parser.add_argument("--validate-metadata", action="store_true", help="validate required TEST-* metadata and exit")
    parser.add_argument("--name", action="append", default=[], help="regex filter over test names; can be repeated")
    parser.add_argument("--only-failed", action="store_true", help="run only tests that failed in the previous saved results")
    parser.add_argument("--rerun-first-failure", action="store_true", help="run only the previous first failure")
    parser.add_argument("--fail-fast", action="store_true", help="stop after the first failure")
    parser.add_argument("--max-failures", type=int, default=None, help="stop after N failures")
    parser.add_argument("--keep-passing-artifacts", action="store_true", help="do not delete temp dirs for passing tests")
    parser.add_argument("--no-preserve-failures", action="store_true", help="do not copy failing artifacts to tests/.last-failures")
    parser.add_argument("--no-cleanup", action="store_true", help="skip final suite artifact cleanup")
    parser.add_argument("--no-color", action="store_true", help="reserved for callers that strip color externally")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.no_color:
        disable_color()
    previous = load_previous_results(RESULTS_FILE)
    all_tests = discover_tests()

    duplicates = find_duplicate_cases(all_tests)
    if duplicates:
        print_duplicate_cases(duplicates)
        return 2

    validation_errors: list[str] = []
    if args.validate_metadata:
        errors = validate_test_metadata(all_tests)
        print_validation_errors("METADATA VALIDATION", errors)
        validation_errors.extend(errors)
    if args.validate_context_links:
        errors = validate_context_links(all_tests)
        print_validation_errors("CONTEXT LINK VALIDATION", errors)
        validation_errors.extend(errors)
    if args.validate_metadata or args.validate_context_links:
        return 2 if validation_errors else 0

    if args.list_tiers:
        print_tier_summary(all_tests)
        return 0

    if args.list_targets:
        list_targets(all_tests)
        return 0

    tests = filter_cases(all_tests, args)

    if args.list:
        list_cases(tests)
        return 0

    print()
    print_box("RUNNING TEST SUITE")
    print()
    active_filter = " ".join(normalize_menu_tokens(args.filters)) if args.filters else "all"
    active_tiers = format_tier_filter(selected_tiers_from_args(args))
    print(f"Total tests registered: {len(all_tests)}".center(WIDTH + 2))
    print(f"Selected tests:         {len(tests)}".center(WIDTH + 2))
    print(f"Tier filter:            {active_tiers}".center(WIDTH + 2))
    print(f"Menu filter:            {active_filter}".center(WIDTH + 2))
    print()

    if not tests:
        print_section_line("NO TESTS SELECTED", YELLOW, bold=True)
        return 0

    if FAILURE_DIR.exists() and not args.no_preserve_failures:
        shutil.rmtree(FAILURE_DIR)
    if not args.no_preserve_failures:
        FAILURE_DIR.mkdir(parents=True, exist_ok=True)

    runner = Runner(
        tests,
        RunnerOptions(
            filters=normalize_menu_tokens(args.filters),
            fail_fast=args.fail_fast,
            max_failures=args.max_failures,
            keep_passing_artifacts=args.keep_passing_artifacts,
            preserve_failures=not args.no_preserve_failures,
            no_color=args.no_color,
        ),
    )
    suite_start = time.perf_counter_ns()
    global MONAD_TEST_ENV
    with tempfile.TemporaryDirectory(prefix="monadc-tests-") as tmp:
        suite_tmpdir = Path(tmp)
        env = os.environ.copy()
        suite_home = suite_tmpdir / "home"
        suite_home.mkdir(parents=True, exist_ok=True)
        env["HOME"] = str(suite_home)
        env["MONAD_CORE"] = str(ROOT / "core")
        MONAD_TEST_ENV = env
        try:
            for case in tests:
                runner.run(case, suite_tmpdir)
                if runner.should_stop:
                    print_section_line("STOPPING EARLY", YELLOW, bold=True)
                    break
        finally:
            MONAD_TEST_ENV = None
    if runner.current_section is not None:
        runner.print_section_footer(runner.current_section)
    suite_elapsed_ns = time.perf_counter_ns() - suite_start

    save_results(RESULTS_FILE, runner.results, runner.first_failure)
    save_first_failure(FIRST_FAILURE_FILE, runner.first_failure)

    passed = len(runner.results) - runner.failures

    print()
    print_section_summary(runner)
    print()
    print_failure_kind_summary(runner.results)
    print_rule("TEST SUMMARY")
    print()
    print(f"Registered: {len(all_tests):6d} tests".center(WIDTH + 2))
    print(f"Selected:   {len(tests):6d} tests".center(WIDTH + 2))
    print(f"Executed:   {len(runner.results):6d} tests".center(WIDTH + 2))
    print(f"Passed:     {passed:6d} tests".center(WIDTH + 2))
    print(f"Failed:     {runner.failures:6d} tests".center(WIDTH + 2))
    print(f"Time:       {format_duration(suite_elapsed_ns)}".center(WIDTH + 2))

    regressions, fixes = compute_changes(previous, runner.results)
    print_changes(regressions, fixes)
    if not args.no_cleanup:
        cleanup_suite_artifacts()

    print()
    print_result_box(runner.failures == 0)
    return 1 if runner.failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
