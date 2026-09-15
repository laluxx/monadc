import re
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from src.testing.monad_binary import resolve_monad_binary


ROOT = Path(__file__).resolve().parents[3]
CORE = ROOT / "core"
MONAD = resolve_monad_binary()


class CoreApiDryTests(unittest.TestCase):
    def test_layout_fields_use_type_annotations_not_arrows(self):
        offenders = []
        field_arrow = re.compile(
            r"\[[A-Za-z_][A-Za-z0-9_?!-]*\s+->\s+[^]]+\]"
        )
        for path in sorted(CORE.rglob("*.mon")):
            if path.name.startswith(".#"):
                continue
            source = path.read_text(encoding="utf-8")
            for match in field_arrow.finditer(source):
                line = source.count("\n", 0, match.start()) + 1
                if source.rfind(";;", 0, match.start()) > source.rfind("\n", 0, match.start()):
                    continue
                offenders.append(f"{path.relative_to(ROOT)}:{line}")
        self.assertEqual(
            offenders,
            [],
            "Layout fields are type annotations (`field :: Type`), not "
            "quantitative/function arrows:\n" + "\n".join(offenders),
        )

    def test_unary_comparison_guards_do_not_repeat_the_receiver(self):
        offenders = []
        repeated_receiver = re.compile(
            r"^\s+([A-Za-z_][A-Za-z0-9_?!-]*)\s+\|\s+\1\s+(?:[<>=]|!=)",
            re.MULTILINE,
        )
        for path in sorted(CORE.rglob("*.mon")):
            if path.name.startswith(".#"):
                continue
            source = path.read_text(encoding="utf-8")
            for match in repeated_receiver.finditer(source):
                line = source.count("\n", 0, match.start()) + 1
                offenders.append(f"{path.relative_to(ROOT)}:{line}")
        self.assertEqual(
            offenders,
            [],
            "Unary comparison guards are receiver-relative; write `| < 2` "
            "rather than `n | n < 2`:\n" + "\n".join(offenders),
        )

    def test_core_assembly_uses_detached_multiline_bodies(self):
        offenders = []
        for path in sorted(CORE.rglob("*.mon")):
            if path.name.startswith(".#"):
                continue
            for number, line in enumerate(path.read_text().splitlines(), 1):
                if "(asm " in line:
                    offenders.append(
                        f"{path.relative_to(ROOT)}:{number}: {line.strip()}"
                    )
        self.assertEqual(offenders, [], "\n".join(offenders))

    def test_unary_functions_use_direct_pattern_clauses(self):
        redundant_matches = []
        pattern = re.compile(
            r"^\s+([^\n]+?) -> \(match ([A-Za-z_][A-Za-z0-9_?!-]*) with",
            re.MULTILINE,
        )

        for path in sorted(CORE.rglob("*.mon")):
            relative = path.relative_to(CORE)
            if any(part.startswith(".") for part in relative.parts):
                continue
            source = path.read_text(encoding="utf-8")
            for match in pattern.finditer(source):
                parameters = re.findall(
                    r"[A-Za-z_][A-Za-z0-9_?!-]*", match.group(1)
                )
                if match.group(2) not in parameters:
                    continue
                line = source.count("\n", 0, match.start()) + 1
                redundant_matches.append(f"{relative}:{line}")

        self.assertEqual(
            redundant_matches,
            [],
            "A unary argument immediately matched by its function body should "
            "be written as direct pattern clauses:\n"
            + "\n".join(redundant_matches),
        )

    def test_module_local_method_shadows_class_dispatch_inside_type_file(self):
        source = """\
module Probe [Walk nth]

class Walk c where
  nth :: c a -> Int -> a

method nth :: [a] -> Int -> a
  [] _              -> error "nth: index out of bounds"
  [x|xs] n | n <= 0 -> x
  [x|xs] n          -> nth xs (n - 1)

instance Walk Coll
  nth xs n -> nth xs n

tests
  assert-eq (nth (list 4 5 6) 2) 6 "module-local method backs class slot"
"""
        with tempfile.TemporaryDirectory(prefix="monadc-local-method-") as td:
            root = Path(td)
            path = root / "Probe.mon"
            path.write_text(source, encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(root / "home")
            env["MONAD_CORE"] = str(CORE)
            result = subprocess.run(
                [str(MONAD), "test", str(path)],
                cwd=root,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=20,
            )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("module-local method backs class slot", result.stdout)

    def test_public_methods_do_not_repeat_their_owning_abstraction_name(self):
        violations = []
        for path in sorted(CORE.rglob("*.mon")):
            relative = path.relative_to(CORE)
            if any(part.startswith(".") for part in relative.parts):
                continue
            source = path.read_text(encoding="utf-8")
            classes = re.findall(
                r"^class(?:\s*\([^\n]+\)\s*=>)?\s+([A-Za-z][A-Za-z0-9_]*)",
                source,
                re.MULTILINE,
            )
            methods = re.findall(
                r"^method\s+([^\s:]+)", source, re.MULTILINE
            )
            for class_name in classes:
                prefix = class_name.lower() + "-"
                for method_name in methods:
                    if method_name.lower().startswith(prefix):
                        violations.append(
                            f"{relative}: {method_name} repeats {class_name}"
                        )

        self.assertEqual(
            violations,
            [],
            "Owning classes already namespace methods; use generated "
            "__impl_Class_Type_method names only below the source API:\n"
            + "\n".join(violations),
        )


if __name__ == "__main__":
    unittest.main()
