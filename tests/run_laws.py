#!/usr/bin/env python3

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from monad_binary import resolve_monad_binary


ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "core"
MONAD = resolve_monad_binary()


LAW_PROGRAMS = {
    "Eq Bool": """\
import Data.Eq
module CoreEqLaws []
tests
  laws Eq Bool
""",
    "Semigroup Bool": """\
import Data.Eq
import Data.Semigroup
module CoreSemigroupLaws []
tests
  laws Semigroup Bool
""",
    "Monoid Bool": """\
import Data.Eq
import Data.Semigroup
module CoreMonoidLaws []
tests
  laws Monoid Bool
""",
    "Enum Int": """\
import Data.Enum
import Test.QuickCheck
module CoreEnumLaws []
tests
  seeded law Enum Int
""",
    "Ord Int": """\
import Data.Ord
import Test.QuickCheck
module CoreOrdLaws []
tests
  seeded law Ord Int
""",
    "Ring Int": """\
import Numeric
import Test.QuickCheck
module CoreRingLaws []
tests
  seeded law Ring Int
""",
    "Show ShowSample": """\
import Text.Show
module CoreShowLaws []
tests
  laws Show ShowSample
""",
    "Sequence Coll": """\
import Sequence
import Test.QuickCheck
module CoreSequenceLaws []
tests
  seeded law Sequence Coll
""",
}

CLASS_DECLARATION = re.compile(
    r"(?:=>\s*)?([A-Z][A-Za-z0-9]*)\s+[a-z][A-Za-z0-9]*\s+where\s*$"
)


def declared_law_classes() -> set[str]:
    classes: set[str] = set()
    for path in sorted((CORE / "prelude").rglob("*.mon")):
        current_class = None
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("class "):
                match = CLASS_DECLARATION.search(line)
                current_class = match.group(1) if match else None
            elif line.startswith("  law ") and current_class:
                classes.add(current_class)
            elif line and not line[0].isspace():
                current_class = None
    return classes


def validate_inventory() -> bool:
    declared = declared_law_classes()
    covered = {family.split()[0] for family in LAW_PROGRAMS}
    if declared == covered:
        return True
    missing = sorted(declared - covered)
    stale = sorted(covered - declared)
    if missing:
        print(f"law suite is missing core classes: {', '.join(missing)}",
              file=sys.stderr)
    if stale:
        print(f"law suite names classes without laws: {', '.join(stale)}",
              file=sys.stderr)
    return False


def main() -> int:
    if not validate_inventory():
        return 2
    programs = LAW_PROGRAMS
    if len(sys.argv) > 1:
        requested = sys.argv[1]
        if requested not in LAW_PROGRAMS:
            print(f"unknown core law family: {requested}", file=sys.stderr)
            return 2
        programs = {requested: LAW_PROGRAMS[requested]}

    with tempfile.TemporaryDirectory(prefix="monadc-core-laws-") as temp_name:
        temp = Path(temp_name)
        failures = []
        for index, (family, source) in enumerate(programs.items(), 1):
            print(f"\n[{index}/{len(programs)}] {family}", flush=True)
            source_path = temp / f"CoreLaw{index}.mon"
            source_path.write_text(source, encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(temp / f"home-{index}")
            env["MONAD_CORE"] = str(CORE)
            result = subprocess.run(
                [str(MONAD), "test", str(source_path)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=120,
            )
            print(result.stdout, end="")
            if result.returncode:
                print(f"\nCore law family failed: {family}", file=sys.stderr)
                failures.append(family)

    if failures:
        print(
            f"\nCore laws failed: {len(failures)}/{len(programs)} families"
            f" ({', '.join(failures)})",
            file=sys.stderr,
        )
        return 1
    print(f"\nAll core laws passed: {len(programs)} families")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
