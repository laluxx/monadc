#!/usr/bin/env python3
"""Compiler-facing test-suite orchestration.

Authored language tests live exclusively in ``tests/**/*.mon``.  This module is
host infrastructure: it composes the canonical Monad runner with the smaller
ABI, portability, build-system, and tooling contracts that cannot honestly be
expressed as Monad programs yet.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from src.tooling.console import Console, color_enabled

ROOT = Path(__file__).resolve().parents[2]
PYTHON = sys.executable or "python3"
UI = Console(color=color_enabled("always"), project="monad")


@dataclass(frozen=True)
class Suite:
    name: str
    description: str
    commands: tuple[tuple[str, ...], ...]


def py(module: str, *args: str) -> tuple[str, ...]:
    return (PYTHON, "-B", "-m", module, *args)


SUITES: dict[str, Suite] = {
    "runner": Suite(
        "runner",
        "Host harness, portability, CLI, examples, REPL, and bytecode contracts.",
        (
            py("src.testing.contracts.test_run"),
            py("src.testing.contracts.test_run_core"),
            py("src.testing.contracts.test_cli_duality"),
            py("src.testing.contracts.test_tuple_commas"),
            py("src.testing.contracts.test_windows_portability"),
            py("src.testing.contracts.test_cmake_build"),
            py("src.testing.contracts.test_readme_product"),
            py("src.testing.contracts.test_unified_test_entrypoint"),
            py("src.testing.contracts.test_repl"),
            py("src.testing.contracts.test_tail_calls"),
            py("src.testing.contracts.test_how_to_examples"),
            py("src.testing.contracts.test_bytecode"),
        ),
    ),
    "core": Suite(
        "core",
        "Core and prelude module tests discovered from active tests blocks.",
        (py("src.testing.core_runner"),),
    ),
    "laws": Suite(
        "laws",
        "Execute the authored Monad verification corpus, including structured law families.",
        (py("src.testing.runner"),),
    ),
    "how-to": Suite(
        "how-to",
        "Compile README-listed how_to examples with checkout-local core/runtime paths.",
        (py("src.testing.contracts.test_how_to_examples"),),
    ),
    "windows": Suite(
        "windows",
        "MSYS2/Windows portability contracts and checkout-local executable resolution.",
        (
            py("src.testing.contracts.test_windows_portability"),
            py("src.testing.contracts.test_checkout_local_paths"),
        ),
    ),
    "cmake": Suite(
        "cmake",
        "CMake and CI integration contracts.",
        (py("src.testing.contracts.test_cmake_build"),),
    ),
    "readme": Suite(
        "readme",
        "Human-facing README product contract.",
        (py("src.testing.contracts.test_readme_product"),),
    ),
    "bytecode": Suite(
        "bytecode",
        "Bytecode VM, verifier, serialization, and visual diagnostics.",
        (py("src.testing.contracts.test_bytecode"),),
    ),
}


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="monad test",
        description="Run Monad language verification and host integration suites.",
    )
    parser.add_argument(
        "suite",
        nargs="?",
        default="list",
        help="suite to run, 'all', or 'list' to show the menu",
    )
    return parser.parse_args(argv)


def list_suites() -> int:
    UI.header("test", "language verification · host contracts")
    UI.section("Available suites")
    for name in sorted(SUITES):
        UI.row(name, SUITES[name].description)
    UI.hint("run: monad test <suite>")
    return 0


def run_command(command: tuple[str, ...]) -> int:
    env = os.environ.copy()
    # Monad source and diagnostics are UTF-8 on every supported host.  Python
    # otherwise inherits a legacy Windows code page in some CI shells.
    env["PYTHONUTF8"] = "1"
    module = command[command.index("-m") + 1] if "-m" in command else Path(command[0]).name
    UI.step(module)
    result = subprocess.run(command, cwd=ROOT, env=env, check=False)
    if result.returncode:
        UI.fail(f"{module} exited with status {result.returncode}")
    return result.returncode


def run_suite(suite: Suite) -> int:
    UI.header("test", suite.description)
    UI.section(suite.name)
    UI.row("contracts", str(len(suite.commands)))
    started = time.perf_counter()
    for command in suite.commands:
        code = run_command(command)
        if code:
            return code
    UI.ok(f"{suite.name} passed in {time.perf_counter() - started:.2f}s")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
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
        UI.header("test", "language verification · host contracts")
        UI.fail(f"unknown suite: {args.suite}")
        UI.hint("run 'monad test list' to see available suites")
        return 2
    return run_suite(suite)


if __name__ == "__main__":
    raise SystemExit(main())
