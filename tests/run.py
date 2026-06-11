#!/usr/bin/env python3

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
WIDTH = 78

RESET      = "\033[0m"
BOLD_GREEN = "\033[1;32m"
BOLD_RED   = "\033[1;31m"
GREEN      = "\033[32m"
YELLOW     = "\033[33m"
GRAY       = "\033[90m"
CYAN       = "\033[36m"


@dataclass
class TestCase:
    name: str
    metadata: dict[str, str]
    fixture: Path | None = None
    source: str | None = None


@dataclass
class TestResult:
    name: str
    passed: bool
    elapsed_ns: int
    message: str = ""
    output: str = ""


class Runner:
    def __init__(self) -> None:
        self.results: list[TestResult] = []

    def run(self, case: TestCase, suite_tmpdir: Path) -> None:
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
        print_result(self.results[-1])

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
        cases.append(TestCase(name=name, fixture=fixture, metadata=metadata))
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
        test_id = f"tests.{prefix}.{name}"
        meta = {
            "TEST-ID": test_id,
            "TEST-CONTEXT": context,
            "TEST-PURPOSE": purpose,
            "TEST-EXPECT": "parse-json",
        }
        if expected:
            meta["TEST-EXPECT-DESUGAR"] = expected
        cases.append(
            TestCase(
                name=f"{prefix}.{name}",
                metadata=meta,
                source=source.replace("\\n", "\n"),
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


def run_case(case: TestCase, tmpdir: Path) -> tuple[bool, str, str]:
    expect = case.metadata.get("TEST-EXPECT", "parse-json")
    if expect == "inventory":
        return True, "", ""

    fixture = materialize_fixture(case, tmpdir)
    output_base = tmpdir / (case.fixture.stem if case.fixture else safe_name(case.name))
    json_path = Path(f"{output_base}.json")
    result = run_monad([str(fixture), "--emit-json", "-o", str(output_base)])
    if result.returncode != 0:
        return False, "--emit-json failed", result.stdout
    if not json_path.is_file():
        return False, f"expected JSON output at {json_path}", result.stdout

    try:
        emitted = json.loads(json_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return False, f"could not read emitted JSON: {error}", ""

    special = run_special_assertions(case, emitted)
    if special:
        return special

    expected_desugar = case.metadata.get("TEST-EXPECT-DESUGAR")
    if expected_desugar:
        actual = extract_desugared_ast(result.stdout)
        if actual != expected_desugar:
            return False, f"desugared AST mismatch: expected {expected_desugar!r}, got {actual!r}", result.stdout

    stdout_path = case.fixture.with_suffix(".stdout") if case.fixture else None
    should_compile = "compile" in expect or "run" in expect or bool(stdout_path and stdout_path.exists())
    if not should_compile:
        return True, "", result.stdout

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


def run_special_assertions(case: TestCase, emitted: object) -> tuple[bool, str, str] | None:
    test_id = case.metadata.get("TEST-ID", "")
    if test_id == "tests.reader.path-heap-literals":
        if not contains_object(emitted, {"type": "path", "value": "~/xos/projects/c/monadc/context"}):
            return False, "JSON did not preserve home-relative Path literal", ""
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
        if path.exists() and path not in before:
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
        stem.with_suffix(".json"),
        stem,
        Path(f"{stem}_test"),
    ]


def cleanup_artifacts(path: Path) -> None:
    for child in path.iterdir():
        if child.is_dir():
            shutil.rmtree(child)
        else:
            child.unlink()


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name)


def print_box(title: str) -> None:
    print(f"╔{'═' * WIDTH}╗")
    print(f"║{title.center(WIDTH)}║")
    print(f"╚{'═' * WIDTH}╝")


def print_rule(title: str) -> None:
    print("═" * (WIDTH + 2))
    print(title.center(WIDTH + 2))
    print("═" * (WIDTH + 2))


def print_result(result: TestResult) -> None:
    status = f"{GREEN}✓{RESET} {BOLD_GREEN}PASS{RESET}" if result.passed else f"{BOLD_RED}✗ FAIL{RESET}"
    timing = format_duration(result.elapsed_ns)
    prefix = f"-{YELLOW}▶{RESET}  {result.name} "
    suffix = f" {status} {timing}"
    plain_prefix = f"-▶  {result.name} "
    plain_status = "✓ PASS" if result.passed else "✗ FAIL"
    plain_suffix = f" {plain_status} {strip_ansi(timing)}"
    dots = "." * max(1, WIDTH + 2 - len(plain_prefix) - len(plain_suffix))
    print(f"{prefix}{GRAY}{dots}{RESET}{suffix}")
    if not result.passed:
        if result.message:
            print(f"    reason: {result.message}")
        if result.output:
            print(indent_output(result.output))


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
    for code in (RESET, BOLD_GREEN, BOLD_RED, GREEN, YELLOW, GRAY, CYAN):
        value = value.replace(code, "")
    return value


def indent_output(output: str) -> str:
    return "\n".join(f"    {line}" for line in output.rstrip().splitlines())


def main() -> int:
    tests = discover_tests()

    print()
    print_box("RUNNING TEST SUITE")
    print()
    print(f"Total tests registered: {len(tests)}".center(WIDTH + 2))
    print()

    runner = Runner()
    suite_start = time.perf_counter_ns()
    with tempfile.TemporaryDirectory(prefix="monadc-tests-") as tmp:
        suite_tmpdir = Path(tmp)
        for case in tests:
            runner.run(case, suite_tmpdir)
    suite_elapsed_ns = time.perf_counter_ns() - suite_start

    passed = len(runner.results) - runner.failures

    print()
    print_rule("TEST SUMMARY")
    print()
    print(f"Total:  {len(runner.results):6d} tests".center(WIDTH + 2))
    print(f"Passed: {passed:6d} tests".center(WIDTH + 2))
    print(f"Failed: {runner.failures:6d} tests".center(WIDTH + 2))
    print(f"Time:   {format_duration(suite_elapsed_ns)}".center(WIDTH + 2))
    print()

    if runner.failures:
        print_box(f"✗ {runner.failures} TEST(S) FAILED ✗")
        return 1

    print_box("✓ ALL TESTS PASSED ✓")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
