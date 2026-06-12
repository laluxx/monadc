#!/usr/bin/env python3

import difflib
import json
import re
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
TEST_ROOT = ROOT / "tests"
MONAD = ROOT / "monad"
RESULTS_FILE = TEST_ROOT / ".test-results.json"
FIRST_FAILURE_FILE = TEST_ROOT / ".last-first-failure.org"
WIDTH = 78
TRACKED_FILES: set[Path] | None = None

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

SECTION_STYLES = {
    "codegen": (MAGENTA, "Runtime Codegen"),
    "language": (BLUE, "Reader Language Atoms"),
    "sugar": (CYAN, "Reader Sugar Forms"),
}


@dataclass
class TestCase:
    name: str
    metadata: dict[str, str]
    fixture: Path | None = None
    source: str | None = None
    origin: Path | None = None
    origin_line: int = 1
    origin_col: int = 1


@dataclass
class TestResult:
    name: str
    passed: bool
    elapsed_ns: int
    message: str = ""
    output: str = ""


class Runner:
    def __init__(self, cases: list[TestCase]) -> None:
        self.results: list[TestResult] = []
        self.first_failure: tuple[TestCase, TestResult] | None = None
        self.total = len(cases)
        self.progress_prefix_width = progress_prefix_width(cases)
        self.current_section: str | None = None

    def run(self, case: TestCase, suite_tmpdir: Path) -> None:
        section = section_key(case.name)
        if section != self.current_section:
            self.current_section = section
            print_section_header(section)

        start = time.perf_counter_ns()
        test_tmpdir = suite_tmpdir / safe_name(case.name)
        test_tmpdir.mkdir()
        side_effects_before = artifact_snapshot(case.fixture) if case.fixture else set()
        try:
            passed, message, output = run_case(case, test_tmpdir)
            cleanup_artifacts(test_tmpdir)
            if case.fixture:
                cleanup_fixture_artifacts(case.fixture, side_effects_before)
            leftovers = list(test_tmpdir.iterdir())
            if leftovers:
                passed = False
                names = ", ".join(path.name for path in leftovers)
                message = f"test left temporary artifacts after cleanup: {names}"
        finally:
            shutil.rmtree(test_tmpdir, ignore_errors=True)
        elapsed_ns = time.perf_counter_ns() - start
        self.results.append(TestResult(case.name, passed, elapsed_ns, message, output))
        result = self.results[-1]
        if not result.passed and self.first_failure is None:
            self.first_failure = (case, result)
        print_result(result, len(self.results), self.total, case, self.progress_prefix_width)

    @property
    def failures(self) -> int:
        return sum(1 for result in self.results if not result.passed)


def discover_tests() -> list[TestCase]:
    cases: list[TestCase] = []
    for fixture in sorted(TEST_ROOT.rglob("*.mon")):
        metadata = read_metadata(fixture)
        test_id = metadata.get("TEST-ID")
        if not test_id:
            continue
        name = test_id.removeprefix("tests.")
        cases.append(
            TestCase(
                name=name,
                fixture=fixture,
                metadata=metadata,
                origin=fixture,
                origin_line=fixture_start_line(fixture),
                origin_col=1,
            )
        )
    for corpus_name in ("reader-atoms.tsv", "reader-sugars.tsv"):
        corpus = TEST_ROOT / corpus_name
        if corpus.exists():
            prefix = path_to_prefix(corpus_name)
            cases.extend(read_corpus(corpus, prefix))
    return cases


def path_to_prefix(name: str) -> str:
    table = {"reader-atoms.tsv": "language", "reader-sugars.tsv": "sugar"}
    return table.get(name, "language")


def read_corpus(path: Path, prefix: str = "language") -> list[TestCase]:
    cases: list[TestCase] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) < 4:
            raise ValueError(f"{path}:{line_number}: expected at least 4 tab-separated fields")
        name, context, purpose, source = fields[:4]
        expected = fields[4].strip() if len(fields) >= 5 else ""
        expected_diagnostic = fields[5].strip() if len(fields) >= 6 else ""
        test_id = f"tests.{prefix}.{name}"
        meta = {
            "TEST-ID": test_id,
            "TEST-CONTEXT": context,
            "TEST-PURPOSE": purpose,
            "TEST-EXPECT": "parse-json",
        }
        if expected:
            if expected.startswith("fail:"):
                meta["TEST-EXPECT"] = expected
                if expected_diagnostic:
                    meta["TEST-EXPECT-DIAGNOSTIC"] = expected_diagnostic
            else:
                meta["TEST-EXPECT-DESUGAR"] = expected
        cases.append(
            TestCase(
                name=f"{prefix}.{name}",
                metadata=meta,
                source=source.replace("\\n", "\n"),
                origin=path,
                origin_line=line_number,
                origin_col=1,
            )
        )
    return cases


def read_metadata(path: Path) -> dict[str, str]:
    metadata: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith(";;"):
            if line.strip():
                break
            continue
        match = re.match(r";;\s*(TEST-[A-Z-]+):\s*(.*)", line)
        if match:
            metadata[match.group(1)] = match.group(2).strip()
    return metadata


def fixture_start_line(path: Path) -> int:
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith(";;"):
            continue
        return line_number
    return 1


def run_case(case: TestCase, tmpdir: Path) -> tuple[bool, str, str]:
    expect = case.metadata.get("TEST-EXPECT", "parse-json")
    if expect == "inventory":
        return True, "", ""

    fixture = materialize_fixture(case, tmpdir)
    output_base = tmpdir / (case.fixture.stem if case.fixture else safe_name(case.name))
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

        exe = tmpdir / (case.fixture.stem if case.fixture else safe_name(case.name))
        result = run_monad([str(fixture), "-o", str(exe)])
        if result.returncode == 0:
            return False, f"expected {expect} but compile succeeded", result.stdout
        if stdout_path and stdout_path.exists():
            expected = stdout_path.read_text(encoding="utf-8")
            actual = normalize_golden_output(result.stdout)
            if expected not in actual:
                return False, f"diagnostic did not contain {stdout_path.relative_to(ROOT)}", result.stdout
        expected_diagnostic = case.metadata.get("TEST-EXPECT-DIAGNOSTIC")
        if expected_diagnostic and expected_diagnostic not in result.stdout:
            return False, f"diagnostic did not contain {expected_diagnostic!r}", result.stdout
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

    expected_desugar = case.metadata.get("TEST-EXPECT-DESUGAR")
    if expected_desugar:
        actual = extract_desugared_ast(emit_stdout)
        if actual != expected_desugar:
            return False, f"desugared AST mismatch: expected {expected_desugar!r}, got {actual!r}", emit_stdout

    should_compile = "compile" in expect or "run" in expect or bool(stdout_path and stdout_path.exists())
    if not should_compile:
        return True, "", emit_stdout

    exe = tmpdir / case.fixture.stem
    if case.fixture is None:
        exe = tmpdir / safe_name(case.name)
    result = run_monad([str(fixture), "-o", str(exe)])
    if result.returncode != 0:
        return False, "compile failed", result.stdout
    if not exe.is_file() or not exe.stat().st_mode & 0o111:
        return False, f"expected executable at {exe}", result.stdout

    if "run" not in expect and not stdout_path.exists():
        return True, "", result.stdout

    result = subprocess.run(
        [str(exe)],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        return False, f"executable failed with exit code {result.returncode}", result.stdout

    if stdout_path and stdout_path.exists():
        expected = stdout_path.read_text(encoding="utf-8")
        if not result.stdout.endswith(expected):
            return False, f"stdout did not end with {stdout_path.relative_to(ROOT)}", result.stdout

    return True, "", result.stdout


def materialize_fixture(case: TestCase, tmpdir: Path) -> Path:
    if case.fixture:
        return case.fixture
    if case.source is None:
        raise ValueError(f"{case.name}: missing fixture and source")
    path = tmpdir / f"{safe_name(case.name)}.mon"
    path.write_text(case.source.rstrip() + "\n", encoding="utf-8")
    return path


def emit_and_check_reader_goldens(
    case: TestCase,
    fixture: Path,
    output_base: Path,
    json_golden_path: Path | None,
    desugar_golden_path: Path | None,
) -> tuple[tuple[bool, str, str] | None, object | None, str]:
    json_path = Path(f"{output_base}.json")
    result = run_monad([str(fixture), "--emit-json", "-o", str(output_base)])
    if result.returncode != 0:
        return (False, "--emit-json failed", result.stdout), None, result.stdout
    if not json_path.is_file():
        return (False, f"expected JSON output at {json_path}", result.stdout), None, result.stdout

    try:
        emitted = json.loads(json_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return (False, f"could not read emitted JSON: {error}", ""), None, result.stdout

    if json_golden_path and json_golden_path.exists():
        mismatch = compare_json_golden(json_golden_path, emitted)
        if mismatch:
            return (False, mismatch, result.stdout), None, result.stdout

    if desugar_golden_path and desugar_golden_path.exists():
        expected = desugar_golden_path.read_text(encoding="utf-8").strip()
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


def compare_json_golden(path: Path, emitted: object) -> str | None:
    try:
        expected = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return f"could not read expected AST JSON {path.relative_to(ROOT)}: {error}"

    expected_norm = normalize_ast_json(expected)
    emitted_norm = normalize_ast_json(emitted)
    if expected_norm == emitted_norm:
        return None

    expected_text = json.dumps(expected_norm, indent=2, sort_keys=True).splitlines()
    emitted_text = json.dumps(emitted_norm, indent=2, sort_keys=True).splitlines()
    diff = unified_diff_text(
        expected_text,
        emitted_text,
        str(path.relative_to(ROOT)),
        "actual AST JSON",
    )
    return f"AST JSON did not match {path.relative_to(ROOT)}\n{diff}"


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


def run_special_assertions(case: TestCase, emitted: object) -> tuple[bool, str, str] | None:
    test_id = case.metadata.get("TEST-ID", "")
    if test_id == "tests.reader.heap-literals":
        if not contains_object(emitted, {"type": "array", "is_heap": True}):
            return False, "JSON did not mark ~[1 2 3] as a heap array", ""
    return None


def extract_desugared_ast(output: str) -> str:
    lines = output.split("\n")
    in_block = False
    parts = []
    for line in lines:
        if line.startswith("=== desugared AST"):
            in_block = True
            continue
        if in_block:
            if line.startswith("=== end desugared AST"):
                break
            if line.startswith("[compile]") or line.startswith("  wrote"):
                continue
            if line.strip():
                parts.append(line.strip())
    return "\n".join(parts)


def run_monad(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(MONAD), *args],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def normalize_golden_output(output: str) -> str:
    return output.replace(str(ROOT), "<repo>")


def contains_object(value: object, expected: dict[str, object]) -> bool:
    if isinstance(value, dict):
        if all(value.get(key) == expected_value for key, expected_value in expected.items()):
            return True
        return any(contains_object(child, expected) for child in value.values())
    if isinstance(value, list):
        return any(contains_object(child, expected) for child in value)
    return False


def artifact_snapshot(fixture: Path) -> set[Path]:
    return {path for path in artifact_candidates(fixture) if path.exists()}


def cleanup_fixture_artifacts(fixture: Path, before: set[Path]) -> None:
    for path in artifact_candidates(fixture):
        if path.exists() and path not in tracked_files():
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


def tracked_files() -> set[Path]:
    global TRACKED_FILES
    if TRACKED_FILES is None:
        result = subprocess.run(
            ["git", "ls-files", "tests"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        TRACKED_FILES = {
            (ROOT / line).resolve()
            for line in result.stdout.splitlines()
            if line.strip()
        }
    return TRACKED_FILES


def cleanup_artifacts(path: Path) -> None:
    for child in path.iterdir():
        if child.is_dir():
            shutil.rmtree(child)
        else:
            child.unlink()


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name)


def section_key(name: str) -> str:
    return name.split(".", 1)[0] if "." in name else "misc"


def print_box(title: str) -> None:
    print(f"╔{'═' * WIDTH}╗")
    print(f"║{title.center(WIDTH)}║")
    print(f"╚{'═' * WIDTH}╝")


def print_rule(title: str) -> None:
    print("═" * (WIDTH + 2))
    print(title.center(WIDTH + 2))
    print("═" * (WIDTH + 2))


def print_section_header(section: str) -> None:
    color, title = SECTION_STYLES.get(section, (YELLOW, section.title()))
    print()
    print(f"{color}{BOLD}┌─ {title} ({section}) {'─' * max(1, WIDTH - len(title) - len(section) - 8)}┐{RESET}")


def print_result(result: TestResult, index: int, total: int, case: TestCase, prefix_width: int) -> None:
    status = f"{BOLD_GREEN}PASS{RESET}" if result.passed else f"{BOLD_RED}FAIL{RESET}"
    timing = format_duration(result.elapsed_ns)
    loc_path, loc_line, loc_col, severity = compilation_location(case, result)
    counter = f"{index:04d}/{total:04d}"
    prefix = progress_prefix(loc_path, loc_line, loc_col, severity, counter, result.name, colored=True)
    plain_prefix = progress_prefix(loc_path, loc_line, loc_col, severity, counter, result.name, colored=False)
    pad = " " * max(2, prefix_width - len(plain_prefix) + 2)
    print(f"{prefix}{pad}{status} {timing}")
    if not result.passed:
        if result.message:
            print(f"    reason: {result.message}")
        if result.output:
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
) -> str:
    counter_text = f"{WHITE}{counter}{RESET}" if colored else counter
    return f"{loc_path}:{loc_line}:{loc_col}: {severity}: [{counter_text}] {name}"


def progress_prefix_width(cases: list[TestCase]) -> int:
    total = len(cases)
    width = 0
    for index, case in enumerate(cases, 1):
        origin = case.origin or case.fixture or TEST_ROOT
        loc_path = str(origin.resolve())
        counter = f"{index:04d}/{total:04d}"
        for severity in ("note", "error"):
            width = max(
                width,
                len(progress_prefix(loc_path, case.origin_line, case.origin_col, severity, counter, case.name, colored=False)),
            )
    return width


def compilation_location(case: TestCase, result: TestResult) -> tuple[str, int, int, str]:
    if not result.passed:
        found = extract_compiler_location(result.output)
        if found:
            return found
    origin = case.origin or case.fixture or TEST_ROOT
    return str(origin.resolve()), case.origin_line, case.origin_col, "note" if result.passed else "error"


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


def short_expect(expect: str) -> str:
    labels = {
        "compile, run": "run",
        "compile": "compile",
        "parse-json": "json",
        "inventory": "inv",
    }
    if expect.startswith("fail:"):
        return expect.replace("fail:", "fail-", 1)
    if expect.startswith("json:"):
        return "json+"
    return labels.get(expect, expect.replace(" ", ""))


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


def load_previous_results(path: Path) -> dict[str, bool]:
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data.get("results", {})
    except (OSError, json.JSONDecodeError):
        return {}


def save_results(path: Path, results: list[TestResult]) -> None:
    data = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "results": {r.name: {"passed": r.passed, "message": r.message} for r in results},
    }
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


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
        f"- Duration: ={strip_ansi(format_duration(result.elapsed_ns))}=",
        f"- Reason: ={result.message or 'failed'}=",
    ]
    if fixture:
        lines.append(f"- Fixture: ={fixture}=")
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


def cleanup_suite_artifacts() -> None:
    if RESULTS_FILE.exists() and RESULTS_FILE.resolve() not in tracked_files():
        RESULTS_FILE.unlink()
    for fixture in TEST_ROOT.rglob("*.mon"):
        cleanup_fixture_artifacts(fixture, set())


def compute_changes(
    previous: dict[str, bool],
    results: list[TestResult],
) -> tuple[list[str], list[str]]:
    current = {r.name: r.passed for r in results}
    regressions = [name for name, passed in current.items() if not passed and previous.get(name, True)]
    fixes = [name for name, passed in current.items() if passed and name in previous and not previous[name]]
    return sorted(regressions), sorted(fixes)


def print_changes(regressions: list[str], fixes: list[str]) -> None:
    if not regressions and not fixes:
        return
    print()
    BOLD_RED = "\033[1;31m"
    BOLD_GREEN = "\033[1;32m"
    YELLOW = "\033[33m"
    RESET = "\033[0m"
    if regressions:
        print(f"  {BOLD_RED}REGRESSIONS (pass → fail):{RESET}")
        for name in regressions:
            print(f"    {BOLD_RED}✗{RESET} {name}")
    if fixes:
        print(f"  {BOLD_GREEN}FIXES (fail → pass):{RESET}")
        for name in fixes:
            print(f"    {BOLD_GREEN}✓{RESET} {name}")


def main() -> int:
    previous = load_previous_results(RESULTS_FILE)
    tests = discover_tests()

    print()
    print_box("RUNNING TEST SUITE")
    print()
    print(f"Total tests registered: {len(tests)}".center(WIDTH + 2))
    print()

    runner = Runner(tests)
    suite_start = time.perf_counter_ns()
    with tempfile.TemporaryDirectory(prefix="monadc-tests-") as tmp:
        suite_tmpdir = Path(tmp)
        for case in tests:
            runner.run(case, suite_tmpdir)
    suite_elapsed_ns = time.perf_counter_ns() - suite_start

    save_results(RESULTS_FILE, runner.results)
    save_first_failure(FIRST_FAILURE_FILE, runner.first_failure)

    passed = len(runner.results) - runner.failures

    print()
    print_rule("TEST SUMMARY")
    print()
    print(f"Total:  {len(runner.results):6d} tests".center(WIDTH + 2))
    print(f"Passed: {passed:6d} tests".center(WIDTH + 2))
    print(f"Failed: {runner.failures:6d} tests".center(WIDTH + 2))
    print(f"Time:   {format_duration(suite_elapsed_ns)}".center(WIDTH + 2))
    print()

    regressions, fixes = compute_changes(previous, runner.results)
    print_changes(regressions, fixes)
    cleanup_suite_artifacts()

    if runner.failures:
        print_box(f"✗ {runner.failures} TEST(S) FAILED ✗")
        return 1

    print_box("✓ ALL TESTS PASSED ✓")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
