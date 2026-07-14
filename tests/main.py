#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PYTHON = sys.executable or "python3"


@dataclass(frozen=True)
class Suite:
    name: str
    description: str
    commands: tuple[tuple[str, ...], ...]


def py(script: str, *args: str) -> tuple[str, ...]:
    return (PYTHON, script, *args)


SUITES: dict[str, Suite] = {
    "runner": Suite(
        "runner",
        "Python harness contracts, portability checks, example smokes, and bytecode tests.",
        (
            py("tests/test_run.py"),
            py("tests/test_run_core.py"),
            py("tests/test_cli_duality.py"),
            py("tests/test_tuple_commas.py"),
            py("tests/test_windows_portability.py"),
            py("tests/test_cmake_build.py"),
            py("tests/test_checkout_local_paths.py"),
            py("tests/test_readme_product.py"),
            py("tests/test_unified_test_entrypoint.py"),
            py("tests/test_how_to_examples.py"),
            py("tests/test_bytecode.py"),
        ),
    ),
    "core": Suite(
        "core",
        "Core and prelude module tests discovered from active tests blocks.",
        (py("tests/run_core.py"),),
    ),
    "how-to": Suite(
        "how-to",
        "Compile every README-listed how_to example with checkout-local core/runtime paths.",
        (py("tests/test_how_to_examples.py"),),
    ),
    "windows": Suite(
        "windows",
        "MSYS2/Windows portability contracts and checkout-local executable resolution.",
        (
            py("tests/test_windows_portability.py"),
            py("tests/test_checkout_local_paths.py"),
        ),
    ),
    "cmake": Suite(
        "cmake",
        "CMake and CI contract tests.",
        (py("tests/test_cmake_build.py"),),
    ),
    "readme": Suite(
        "readme",
        "Human-facing README product contract.",
        (py("tests/test_readme_product.py"),),
    ),
    "bytecode": Suite(
        "bytecode",
        "Bytecode VM, verifier, serialization, and visual diagnostics.",
        (py("tests/test_bytecode.py"),),
    ),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="tests/main.py",
        description="Unified test entry point for Monad compiler test suites.",
    )
    parser.add_argument(
        "suite",
        nargs="?",
        default="list",
        help="Suite to run, or 'list' to show available suites.",
    )
    return parser.parse_args()


def list_suites() -> int:
    print("Available test suites:")
    width = max(len(name) for name in SUITES)
    for name in sorted(SUITES):
        print(f"  {name:<{width}}  {SUITES[name].description}")
    return 0


def run_command(command: tuple[str, ...]) -> int:
    env = os.environ.copy()
    printable = " ".join(command)
    print(f"\n==> {printable}", flush=True)
    result = subprocess.run(command, cwd=ROOT, env=env, check=False)
    if result.returncode:
        print(f"<== failed: {printable} ({result.returncode})", flush=True)
    return result.returncode


def run_suite(suite: Suite) -> int:
    print(f"Running suite: {suite.name}")
    print(suite.description)
    for command in suite.commands:
        code = run_command(command)
        if code:
            return code
    print(f"\nSuite passed: {suite.name}")
    return 0


def main() -> int:
    args = parse_args()
    if args.suite == "list":
        return list_suites()
    if args.suite == "all":
        for name in ("runner", "core"):
            code = run_suite(SUITES[name])
            if code:
                return code
        return 0
    suite = SUITES.get(args.suite)
    if suite is None:
        print(f"unknown suite: {args.suite}", file=sys.stderr)
        list_suites()
        return 2
    return run_suite(suite)


if __name__ == "__main__":
    raise SystemExit(main())
