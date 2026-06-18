#!/usr/bin/env python3

import os
import re
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CORE_ROOT = ROOT / "core"
MONAD = ROOT / "monad"
WIDTH = 78
TRACKED_FILES: set[Path] | None = None


@dataclass
class ModuleResult:
    path: Path
    passed: bool
    elapsed_ns: int
    output: str
    assertions: int


def discover_modules() -> list[Path]:
    return [
        path
        for path in sorted(CORE_ROOT.rglob("*.mon"))
        if path.is_file()
        if has_active_tests_block(path)
    ]


def has_active_tests_block(path: Path) -> bool:
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.lstrip()
        if stripped.startswith(";;"):
            continue
        if re.match(r"^\(?tests(?:\s|$)", stripped):
            return True
    return False


def count_assertions(path: Path) -> int:
    count = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.lstrip()
        if stripped.startswith(";;"):
            continue
        if re.match(r"^\(?assert-eq(?:\s|\()", stripped):
            count += 1
    return count


def run_module(path: Path) -> ModuleResult:
    before = artifact_snapshot(path)
    start = time.perf_counter_ns()
    env = os.environ.copy()
    with tempfile.TemporaryDirectory(prefix="monadc-core-home-") as home:
        home_path = Path(home)
        temp_core = home_path / "core"
        materialize_core_tree(temp_core)
        rel = path.relative_to(CORE_ROOT)
        test_file = temp_core / rel
        env["MONAD_CORE"] = str(temp_core)
        env["HOME"] = home
        result = subprocess.run(
            [str(MONAD), "test", str(test_file)],
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    elapsed_ns = time.perf_counter_ns() - start
    cleanup_artifacts(path, before)
    return ModuleResult(path, result.returncode == 0, elapsed_ns, result.stdout, count_assertions(path))


def materialize_core_tree(destination: Path) -> None:
    for source in sorted(CORE_ROOT.rglob("*")):
        rel = source.relative_to(CORE_ROOT)
        target = destination / rel
        if source.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.suffix == ".mon":
            target.write_text(materialize_test_source(source), encoding="utf-8")
        else:
            shutil.copy2(source, target)


def materialize_test_source(path: Path) -> str:
    lines = path.read_text(encoding="utf-8").splitlines()
    out: list[str] = []
    assertions: list[str] = []
    in_tests = False
    test_indent = 0

    for line in lines:
        stripped = line.lstrip()
        indent = len(line) - len(stripped)
        if not in_tests and stripped == "tests":
            assertions = []
            in_tests = True
            test_indent = indent
            continue
        if in_tests:
            if stripped and indent <= test_indent and not stripped.startswith(";;"):
                out.append(format_tests_block(assertions))
                in_tests = False
            else:
                converted = convert_test_line(stripped)
                if converted:
                    assertions.append(converted)
                continue
        out.append(line)

    if in_tests:
        out.append(format_tests_block(assertions))
    return "\n".join(out) + "\n"


def format_tests_block(assertions: list[str]) -> str:
    if not assertions:
        return "(tests)"
    return f"(tests {' '.join(assertions)})"


def convert_test_line(stripped: str) -> str:
    if not stripped or stripped.startswith(";;"):
        return ""
    if not stripped.startswith("assert-eq "):
        return stripped
    value = stripped.removeprefix("assert-eq ").strip()
    if value.endswith(")"):
        value = value[:-1].rstrip()
    parts = split_top_level(value, 3)
    if len(parts) != 3:
        return f";; malformed core test: {stripped}"
    return f"(assert-eq {parts[0]} {parts[1]} {parts[2]})"


def split_top_level(value: str, limit: int) -> list[str]:
    parts: list[str] = []
    start = 0
    depth = 0
    in_string = False
    escape = False
    for index, char in enumerate(value):
        if in_string:
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == "\"":
                in_string = False
            continue
        if char == "\"":
            in_string = True
            continue
        if char in "([{":
            depth += 1
            continue
        if char in ")]}":
            depth -= 1
            continue
        if char.isspace() and depth == 0:
            token = value[start:index].strip()
            if token:
                parts.append(token)
                if len(parts) == limit - 1:
                    rest = value[index:].strip()
                    if rest:
                        parts.append(rest)
                    return parts
            start = index + 1
    tail = value[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def artifact_snapshot(path: Path) -> set[Path]:
    return {candidate for candidate in artifact_candidates(path) if candidate.exists()}


def artifact_candidates(path: Path) -> list[Path]:
    stem = path.with_suffix("")
    return [
        stem,
        Path(f"{stem}_test"),
        stem.with_suffix(".o"),
        Path(f"{stem}_test.o"),
        stem.with_suffix(".ll"),
        stem.with_suffix(".bc"),
        stem.with_suffix(".s"),
        stem.with_suffix(".json"),
    ]


def cleanup_artifacts(path: Path, before: set[Path]) -> None:
    for candidate in artifact_candidates(path):
        if candidate.exists() and candidate.resolve() not in tracked_files():
            if candidate.is_dir():
                shutil.rmtree(candidate)
            else:
                candidate.unlink()


def tracked_files() -> set[Path]:
    global TRACKED_FILES
    if TRACKED_FILES is None:
        result = subprocess.run(
            ["git", "ls-files", "core"],
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


def format_duration(elapsed_ns: int) -> str:
    if elapsed_ns < 1_000_000:
        return f"{elapsed_ns / 1_000:7.2f} us"
    if elapsed_ns < 1_000_000_000:
        return f"{elapsed_ns / 1_000_000:7.2f} ms"
    return f"{elapsed_ns / 1_000_000_000:7.2f} s"


def print_box(title: str) -> None:
    print(f"╭{'─' * WIDTH}╮")
    print(f"│{title.center(WIDTH)}│")
    print(f"╰{'─' * WIDTH}╯")


def print_result(result: ModuleResult) -> None:
    name = str(result.path.relative_to(ROOT))
    status = "PASS" if result.passed else "FAIL"
    marker = "✓" if result.passed else "✗"
    suffix = f" {status} [{format_duration(result.elapsed_ns)}] ({result.assertions} assertions)"
    dots = "·" * max(1, WIDTH + 2 - len("   ") - len(name) - len(suffix))
    print(f"{marker}  {name} {dots}{suffix}")
    if not result.passed:
        print_failure(result.output)


def print_failure(output: str) -> None:
    diagnostics = extract_diagnostics(output)
    for line in diagnostics:
        if looks_like_file_diagnostic(line):
            print(line)
        else:
            print(f"   {line}")


def looks_like_file_diagnostic(line: str) -> bool:
    return bool(re.match(r"^(?:/|\.?[^:\s]+/)[^:\n]+:\d+:\d+:\s", line))


def extract_diagnostics(output: str) -> list[str]:
    clean = strip_ansi(output)
    lines = clean.rstrip().splitlines()
    diagnostics: list[str] = []
    seen: set[str] = set()

    for index, line in enumerate(lines):
        if is_primary_diagnostic(line):
            block = collect_diagnostic_block(lines, index)
            for item in block:
                if item not in seen:
                    diagnostics.append(item)
                    seen.add(item)
            if len(diagnostics) >= 12:
                break

    if not diagnostics:
        for line in reversed(lines):
            stripped = line.strip()
            if stripped and not is_trace_line(stripped):
                diagnostics.append(stripped)
                break

    return diagnostics[:14] or ["module failed without diagnostic output"]


def is_primary_diagnostic(line: str) -> bool:
    stripped = line.strip()
    return (
        " error:" in stripped
        or stripped.startswith("error:")
        or "Type inference failed" in stripped
        or "Dependent Type Error" in stripped
        or stripped == "test build failed"
        or stripped.startswith("[error]")
    )


def collect_diagnostic_block(lines: list[str], start: int) -> list[str]:
    block: list[str] = []
    for line in lines[start:start + 8]:
        stripped = line.strip()
        if not stripped:
            if block:
                break
            continue
        if is_trace_line(stripped) and block:
            break
        block.append(stripped)
        if stripped == "test build failed":
            break
    return block


def is_trace_line(line: str) -> bool:
    prefixes = (
        "│", "├", "└", "DEBUG ", "DEFINED ", "=== ", "[dep]",
        "wrote object:", "module:",
    )
    return line.startswith(prefixes)


def strip_ansi(value: str) -> str:
    return re.sub(r"\x1b\[[0-9;]*m", "", value)


def main() -> int:
    modules = discover_modules()

    print()
    print_box("RUNNING CORE AND PRELUDE MODULE TESTS")
    print()
    print(f"Modules with active tests blocks: {len(modules)}".center(WIDTH + 2))
    print()

    results = []
    start = time.perf_counter_ns()
    for module in modules:
        result = run_module(module)
        results.append(result)
        print_result(result)
    elapsed_ns = time.perf_counter_ns() - start

    failures = [result for result in results if not result.passed]
    passed = len(results) - len(failures)
    assertion_total = sum(result.assertions for result in results)

    print()
    print(f"╭{'─' * WIDTH}╮")
    print("CORE TEST SUMMARY".center(WIDTH + 2))
    print(f"╰{'─' * WIDTH}╯")
    print()
    print(f"Total:  {len(results):6d} modules".center(WIDTH + 2))
    print(f"Checks: {assertion_total:6d} assertions".center(WIDTH + 2))
    print(f"Passed: {passed:6d} modules".center(WIDTH + 2))
    print(f"Failed: {len(failures):6d} modules".center(WIDTH + 2))
    print(f"Time:   {format_duration(elapsed_ns)}".center(WIDTH + 2))
    print()

    if failures:
        print_box(f"{len(failures)} CORE/PRELUDE MODULE(S) FAILED")
        return 1

    print_box("ALL CORE/PRELUDE MODULE TESTS PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
