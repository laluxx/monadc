"""Authoritative LLVM consumption of verified QTT lexical cleanup."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


from src.testing.monad_binary import resolve_monad_binary
ROOT = Path(__file__).resolve().parents[3]


class QttProductionMemoryTests(unittest.TestCase):
    def test_polymorphic_export_round_trips_at_two_imported_types(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(module PolyProvider [identity])
(define identity (lambda (x) x))
"""
        consumer = """
(module Main)
(import PolyProvider [identity])
(show (identity 42))
(show (identity "hello"))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "PolyProvider.mon"
            program.write_text(source)
            main = directory / "Main.mon"
            main.write_text(consumer)
            output = directory / "polymorphic-consumer"
            env = os.environ.copy()
            env["HOME"] = str(directory / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(resolve_monad_binary()), str(main), "-o", str(output)],
                cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            interface = next(
                path for path in directory.rglob("*.mqti")
                if "MODULE PolyProvider" in path.read_text()
            ).read_text()
            self.assertIn("MONAD-QTT-INTERFACE 12", interface)
            self.assertIn(
                "CONTRACT identity parameters=0 fingerprint=0000000000000000 "
                "ownership=0", interface)
            self.assertRegex(
                interface,
                r"HM checksum=[0-9a-f]{16} payload=monad-hm-scheme-v1\|"
                r"[0-9a-f]{16}\|1\|",
            )
            executed = subprocess.run(
                [str(output)], cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip().splitlines(),
                             ["42", "hello"])

    def test_projected_result_contract_is_reclaimed_by_caller(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(module QttProjectedCall)
(layout Owner [name :: String] [spare :: String])
(define take-name
  (lambda ([_ : Int] -> String)
    (with [owner (Owner "Ada" "unused")]
      owner.name)))
(define reclaim-name
  (lambda (-> Int)
    (with [name (take-name 0)]
      23)))
(show (reclaim-name))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttProjectedCall.mon"
            output = directory / "qtt-projected-call"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn(
                "contract take-name: result=owned/fresh",
                compiled.stdout,
            )
            reclaim_trace = compiled.stdout[
                compiled.stdout.index("definition reclaim-name"):
                compiled.stdout.index(
                    "verdict:", compiled.stdout.index(
                        "definition reclaim-name")) + 120
            ]
            self.assertIn(
                "ownership ANF: valid", reclaim_trace, compiled.stdout)
            self.assertIn("op i", reclaim_trace)
            self.assertIn("call", reclaim_trace)
            self.assertIn("drop", reclaim_trace)
            self.assertIn("verdict: VERIFIED", reclaim_trace)
            ir = program.with_suffix(".ll").read_text()
            take_start = ir.index("define ptr @take-name")
            take_end = ir.index("\n}", take_start)
            take_function = ir[take_start:take_end]
            reclaim_start = ir.index("define i64 @reclaim-name")
            reclaim_end = ir.index("\n}", reclaim_start)
            reclaim_function = ir[reclaim_start:reclaim_end]
            self.assertEqual(take_function.count("call ptr @strdup"), 2)
            self.assertEqual(take_function.count("call void @free"), 2)
            self.assertIn("call ptr @take-name", reclaim_function)
            self.assertEqual(reclaim_function.count("call void @free"), 1)
            self.assertNotIn("call ptr @strdup", reclaim_function)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "23")

    def test_imported_projected_result_contract_reclaims_in_consumer(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        provider = """
(module ProjectedOwner [take-name])
(layout Owner [name :: String] [spare :: String])
(define take-name
  (lambda ([_ : Int] -> String)
    (with [owner (Owner "Ada" "unused")]
      owner.name)))
"""
        consumer = """
(module Main)
(import ProjectedOwner [take-name])
(define reclaim-projected
  (lambda (-> Int)
    (with [name (take-name 0)]
      31)))
(show (reclaim-projected))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "ProjectedOwner.mon").write_text(provider)
            program = directory / "Main.mon"
            output = directory / "qtt-imported-projected"
            program.write_text(consumer)
            env = os.environ.copy()
            env["HOME"] = str(directory / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-all",
                ],
                cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn(
                "imported verified interface contract "
                "ProjectedOwner.take-name",
                compiled.stdout,
            )
            self.assertIn("result=owned/fresh", compiled.stdout)
            self.assertIn(
                "[effects] imported callable contract "
                "ProjectedOwner.take-name as take-name",
                compiled.stdout,
            )
            ir = program.with_suffix(".ll").read_text()
            start = ir.index("define i64 @reclaim-projected")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertIn(
                "call ptr @ProjectedOwner__take-name", function)
            self.assertEqual(function.count("call void @free"), 1)
            self.assertNotIn("call ptr @strdup", function)
            interface = next(
                path for path in directory.rglob("*.mqti")
                if "MODULE ProjectedOwner" in path.read_text()
            )
            self.assertIn(
                "RESULT type=k4; representation=2 mode=1 origin=3",
                interface.read_text(),
            )
            self.assertIn("MONAD-QTT-INTERFACE 12", interface.read_text())
            self.assertIn("payload=monad-hm-scheme-v1|", interface.read_text())
            self.assertRegex(
                interface.read_text(),
                r"EFFECT fingerprint=[0-9a-f]{16} "
                r"checksum=[0-9a-f]{16} "
                r"payload=monad-callable-contract-v1\|",
            )
            executed = subprocess.run(
                [str(output)], cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "31")

    def test_projected_layout_move_uses_masked_structural_cleanup(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(module QttProjectedCleanup)
(layout Owner [name :: String] [spare :: String])
(define take-name
  (lambda ([_ : Int] -> String)
    (with [owner (Owner "Ada" "unused")]
      owner.name)))
(show (take-name 0))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttProjectedCleanup.mon"
            output = directory / "qtt-projected-cleanup"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn("move-place", compiled.stdout)
            definition_trace = compiled.stdout[
                compiled.stdout.index("definition take-name"):
                compiled.stdout.index("module verdict", compiled.stdout.index(
                    "definition take-name"))
            ]
            self.assertIn(
                "portable certificate: valid-after-Core-free",
                definition_trace,
            )
            self.assertIn("[qtt] reclaim owner#", compiled.stdout)
            self.assertIn("mask=1", compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index("define ptr @take-name")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertEqual(function.count("call ptr @strdup"), 2, function)
            self.assertEqual(function.count("call void @free"), 2, function)
            projected = function.index("getelementptr %layout.Owner")
            sibling_free = function.index("call void @free", projected)
            root_free = function.index("call void @free", sibling_free + 1)
            self.assertLess(projected, sibling_free, function)
            self.assertLess(sibling_free, root_free, function)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "Ada")

    def test_verified_owned_string_replacement_frees_displaced_value(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define replace-owned
  (lambda ([_ : Int] -> String)
    (with [text "old allocation"]
      (begin
        (set! text "replacement allocation")
        text))))
(show (replace-owned 0))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryReplace.mon"
            output = directory / "qtt-memory-replace"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            env.pop("MONAD_QTT_MEMORY", None)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index(
                "define ptr @QttMemoryReplace__replace-owned")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertEqual(function.count("call ptr @strdup"), 2, function)
            self.assertEqual(function.count("call void @free"), 1, function)
            first_copy = function.index("call ptr @strdup")
            second_copy = function.index(
                "call ptr @strdup", first_copy + 1)
            old_load = function.index(
                "load ptr, ptr %text", second_copy)
            old_free = function.index("call void @free", old_load)
            replacement_store = function.index(
                "store ptr", old_free)
            self.assertLess(second_copy, old_load, function)
            self.assertLess(old_load, old_free, function)
            self.assertLess(old_free, replacement_store, function)
            self.assertIn(
                "replace text#", compiled.stdout)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(
                executed.stdout.strip(), "replacement allocation")

    def test_repeated_replacement_consumes_each_certificate_once(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define replace-twice
  (lambda ([_ : Int] -> String)
    (with [text "generation zero"]
      (begin
        (set! text "generation one")
        (set! text "generation two")
        text))))
(show (replace-twice 0))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryReplaceTwice.mon"
            output = directory / "qtt-memory-replace-twice"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index(
                "define ptr @QttMemoryReplaceTwice__replace-twice")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertEqual(function.count("call ptr @strdup"), 3, function)
            self.assertEqual(function.count("call void @free"), 2, function)
            self.assertEqual(
                compiled.stdout.count("[qtt] replace text#"), 2,
                compiled.stdout,
            )
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "generation two")

    def test_replaced_value_is_reclaimed_at_scope_exit_when_dead(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define replace-then-drop
  (lambda ()
    (with [text "old"]
      (begin (set! text "new") 19))))
(show (replace-then-drop))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryReplaceDrop.mon"
            output = directory / "qtt-memory-replace-drop"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index(
                "define ptr @QttMemoryReplaceDrop__replace-then-drop")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertEqual(function.count("call ptr @strdup"), 2, function)
            self.assertEqual(function.count("call void @free"), 2, function)
            first_free = function.index("call void @free")
            final_load = function.index(
                "load ptr, ptr %text", first_free)
            final_free = function.index("call void @free", final_load)
            self.assertLess(first_free, final_load, function)
            self.assertLess(final_load, final_free, function)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "19")

    def test_branch_replacement_emits_path_local_destruction(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define replace-branch
  (lambda ([choose : Bool] -> String)
    (with [text "old branch value"]
      (begin
        (if choose
            (set! text "then replacement")
            (set! text "else replacement"))
        text))))
(show (replace-branch True))
(show (replace-branch False))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryReplaceBranch.mon"
            output = directory / "qtt-memory-replace-branch"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            env["MONAD_QTT_SHADOW"] = "1"
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index(
                "define ptr @QttMemoryReplaceBranch__replace-branch")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertEqual(
                function.count("call ptr @strdup"), 3,
                "\n".join(
                    line for line in compiled.stdout.splitlines()
                    if "fallback" in line or "replace-branch" in line) +
                "\n" + function)
            self.assertEqual(function.count("call void @free"), 2, function)
            then_start = function.index("then:")
            else_start = function.index("else:")
            merge_start = function.index("ifmerge:")
            then_arm = function[then_start:else_start]
            else_arm = function[else_start:merge_start]
            self.assertEqual(then_arm.count("call void @free"), 1, then_arm)
            self.assertEqual(else_arm.count("call void @free"), 1, else_arm)
            self.assertEqual(
                compiled.stdout.count("[qtt] replace text#"), 2,
                compiled.stdout,
            )
            shadow = "\n".join(
                line for line in compiled.stdout.splitlines()
                if "[qtt-shadow]" in line or "replace-branch:" in line
            )
            self.assertIn(
                "[qtt-shadow] replace-branch: verified ", shadow, shadow,
            )
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(
                executed.stdout.splitlines(),
                ["then replacement", "else replacement"],
            )

    def test_asymmetric_branch_preserves_or_replaces_one_owner(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define replace-asymmetric
  (lambda ([choose : Bool] -> String)
    (with [text "preserved generation"]
      (begin
        (if choose (set! text "new generation") text)
        text))))
(show (replace-asymmetric True))
(show (replace-asymmetric False))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryReplaceAsymmetric.mon"
            output = directory / "qtt-memory-replace-asymmetric"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            env["MONAD_QTT_SHADOW"] = "1"
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index(
                "define ptr "
                "@QttMemoryReplaceAsymmetric__replace-asymmetric")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertEqual(function.count("call ptr @strdup"), 2, function)
            self.assertEqual(function.count("call void @free"), 1, function)
            then_start = function.index("then:")
            else_start = function.index("else:")
            merge_start = function.index("ifmerge:")
            then_arm = function[then_start:else_start]
            else_arm = function[else_start:merge_start]
            self.assertEqual(then_arm.count("call void @free"), 1, then_arm)
            self.assertNotIn("call void @free", else_arm)
            shadow = "\n".join(
                line for line in compiled.stdout.splitlines()
                if "[qtt-shadow]" in line or "replace-asymmetric:" in line
            )
            self.assertIn(
                "[qtt-shadow] replace-asymmetric: verified ", shadow, shadow,
            )
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(
                executed.stdout.splitlines(),
                ["new generation", "preserved generation"],
            )

    def test_nested_branch_replacements_consume_path_exact_certificates(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define replace-nested
  (lambda ([outer : Bool] [inner : Bool] -> String)
    (with [text "initial generation"]
      (begin
        (if outer
            (if inner
                (set! text "outer/inner true")
                (set! text "outer true, inner false"))
            (set! text "outer false"))
        text))))
(show (replace-nested True True))
(show (replace-nested True False))
(show (replace-nested False True))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryReplaceNested.mon"
            output = directory / "qtt-memory-replace-nested"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            env["MONAD_QTT_SHADOW"] = "1"
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index(
                "define ptr @QttMemoryReplaceNested__replace-nested")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertEqual(function.count("call ptr @strdup"), 4, function)
            self.assertEqual(function.count("call void @free"), 3, function)
            self.assertEqual(
                compiled.stdout.count("[qtt] replace text#"), 3,
                compiled.stdout,
            )
            shadow = "\n".join(
                line for line in compiled.stdout.splitlines()
                if "[qtt-shadow]" in line or "replace-nested:" in line
            )
            self.assertIn(
                "[qtt-shadow] replace-nested: verified ", shadow, shadow,
            )
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(
                executed.stdout.splitlines(),
                [
                    "outer/inner true",
                    "outer true, inner false",
                    "outer false",
                ],
            )

    def test_memory_opt_out_does_not_emit_replacement_free(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define replace-static
  (lambda ([_ : Int] -> String)
    (with [text "static old"]
      (begin (set! text "static new") text))))
(show (replace-static 0))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryReplaceDisabled.mon"
            output = directory / "qtt-memory-replace-disabled"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            env["MONAD_QTT_MEMORY"] = "0"
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index(
                "define ptr @QttMemoryReplaceDisabled__replace-static")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertNotIn("call ptr @strdup", function)
            self.assertNotIn("call void @free", function)
            self.assertNotIn("[qtt] replace text#", compiled.stdout)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "static new")

    def test_uncertified_alias_replacement_falls_back_without_free(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define replace-with-alias
  (lambda ([borrowed : String] -> String)
    (with [text "old"]
      (begin (set! text borrowed) text))))
(show (replace-with-alias "not freshly allocated"))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryReplaceAlias.mon"
            output = directory / "qtt-memory-replace-alias"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index(
                "define ptr @QttMemoryReplaceAlias__replace-with-alias")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertNotIn("call ptr @strdup", function)
            self.assertNotIn("call void @free", function)
            self.assertNotIn("[qtt] replace text#", compiled.stdout)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(
                executed.stdout.strip(), "not freshly allocated")

    def test_fresh_call_replacement_transfers_and_reclaims_old_owner(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define fresh-prefix
  (lambda ([text : String] -> String)
    (__rt_string_take text 3)))
(define replace-from-call
  (lambda ()
    (with [text "old"]
      (begin (set! text (fresh-prefix "abcdef")) text))))
(show (__rt_string_byte (replace-from-call) 0))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryReplaceCallFallback.mon"
            output = directory / "qtt-memory-replace-call-fallback"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index(
                "define ptr "
                "@QttMemoryReplaceCallFallback__replace-from-call")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertIn(
                "call ptr @QttMemoryReplaceCallFallback__fresh-prefix",
                function,
            )
            self.assertEqual(function.count("call ptr @strdup"), 1, function)
            self.assertEqual(function.count("call void @free"), 1, function)
            self.assertEqual(
                compiled.stdout.count("[qtt] replace text#"), 1,
                compiled.stdout,
            )
            call = function.index(
                "call ptr @QttMemoryReplaceCallFallback__fresh-prefix")
            destroy = function.index("call void @free")
            install = function.index("store ptr %calltmp", destroy)
            self.assertLess(call, destroy, function)
            self.assertLess(destroy, install, function)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "97")

    def test_verified_owned_string_binding_emits_and_executes_free(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define reclaim
  (lambda ()
    (with [text "owned by qtt"]
      7)))
(show (reclaim))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemory.mon"
            output = directory / "qtt-memory"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            env.pop("MONAD_QTT_MEMORY", None)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            llvm = program.with_suffix(".ll")
            self.assertTrue(llvm.exists(), compiled.stdout)
            ir = llvm.read_text()
            self.assertIn("call ptr @strdup", ir)
            self.assertIn("call void @free", ir)
            self.assertLess(
                ir.index("call ptr @strdup"),
                ir.index("call void @free"),
            )
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "7")

    def test_multiple_dead_owned_bindings_are_reclaimed(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define reclaim-many
  (lambda ()
    (with [first "first allocation"
           second "second allocation"]
      11)))
(show (reclaim-many))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryMany.mon"
            output = directory / "qtt-memory-many"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            env.pop("MONAD_QTT_MEMORY", None)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            self.assertEqual(ir.count("call ptr @strdup"), 2, ir)
            self.assertEqual(ir.count("call void @free"), 2, ir)
            second_drop = ir.index("load ptr, ptr %second")
            first_drop = ir.index("load ptr, ptr %first")
            self.assertLess(second_drop, first_drop, ir)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "11")

    def test_owned_and_borrowed_alias_bindings_reclaim_one_capability(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define reclaimAlias
  (lambda ()
    (with [owner "owned member"
           alias owner]
      41)))
(show (reclaimAlias))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryAlias.mon"
            output = directory / "qtt-memory-alias"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            self.assertIn(
                "define ptr @QttMemoryAlias__reclaimAlias", ir)
            start = ir.index(
                "define ptr @QttMemoryAlias__reclaimAlias")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertEqual(function.count("call ptr @strdup"), 1, function)
            self.assertEqual(function.count("call void @free"), 1, function)
            self.assertIn("reclaim owner#", compiled.stdout)
            self.assertNotIn("reclaim alias#", compiled.stdout)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "41")

    def test_nested_owned_scopes_reclaim_inner_and_outer_values(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define reclaim-nested
  (lambda ()
    (with [outer "outer allocation"]
      (with [inner "inner allocation"]
        13))))
(show (reclaim-nested))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryNested.mon"
            output = directory / "qtt-memory-nested"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            env.pop("MONAD_QTT_MEMORY", None)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            self.assertEqual(ir.count("call ptr @strdup"), 2, ir)
            self.assertEqual(ir.count("call void @free"), 2, ir)
            inner_drop = ir.index("load ptr, ptr %inner")
            outer_drop = ir.index("load ptr, ptr %outer")
            self.assertLess(inner_drop, outer_drop, ir)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "13")

    def test_dead_fresh_string_call_result_transfers_into_cleanup(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define fresh-prefix
  (lambda ([text : String] -> String)
    (__rt_string_take text 3)))
(define reclaim-call-result
  (lambda ()
    (with [text (fresh-prefix "owned call result")]
      17)))
(show (reclaim-call-result))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryCall.mon"
            output = directory / "qtt-memory-call"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            self.assertIn("call ptr @rt_string_take", ir)
            self.assertIn("call ptr @QttMemoryCall__fresh-prefix", ir)
            self.assertIn("call void @free", ir)
            self.assertNotIn("call ptr @strdup", ir)
            self.assertIn(
                "backend authority: certified ownership cleanup",
                compiled.stdout,
            )
            self.assertLess(
                ir.index("call ptr @QttMemoryCall__fresh-prefix"),
                ir.index("call void @free"),
            )
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "17")

    def test_fresh_call_result_moved_out_is_not_reclaimed_in_callee(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define fresh-prefix
  (lambda ([text : String] -> String)
    (__rt_string_take text 3)))
(define move-call-result
  (lambda ()
    (with [text (fresh-prefix "abcdef")]
      text)))
(show (__rt_string_byte (move-call-result) 0))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttMemoryMoveCall.mon"
            output = directory / "qtt-memory-move-call"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index(
                "define ptr @QttMemoryMoveCall__move-call-result")
            end = ir.index("\n}", start)
            move_function = ir[start:end]
            self.assertIn(
                "call ptr @QttMemoryMoveCall__fresh-prefix",
                move_function,
            )
            self.assertNotIn("call void @free", move_function)
            self.assertNotIn("call ptr @strdup", move_function)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "97", ir)

    def test_imported_fresh_result_uses_provider_ownership_contract(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        provider = """
(module Fresh [fresh-prefix])
(define fresh-prefix
  (lambda ([text : String] -> String)
    (__rt_string_take text 3)))
"""
        consumer = """
(module Main)
(import Fresh [fresh-prefix])
(define reclaim-imported
  (lambda ()
    (with [text (fresh-prefix "abcdef")]
      23)))
(show (reclaim-imported))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "Fresh.mon").write_text(provider)
            program = directory / "Main.mon"
            output = directory / "qtt-memory-imported-call"
            program.write_text(consumer)
            env = os.environ.copy()
            env["HOME"] = str(directory / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-all",
                ],
                cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index(
                "define ptr @reclaim-imported")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertIn(
                "call ptr @Fresh__fresh-prefix",
                function,
            )
            self.assertIn("call void @free", function)
            self.assertNotIn("call ptr @strdup", function)
            self.assertIn(
                "[qtt] imported verified interface contract "
                "Fresh.fresh-prefix",
                compiled.stdout,
            )
            self.assertIn(
                "result=owned/fresh",
                compiled.stdout,
            )
            interfaces = list(directory.rglob("*.mqti"))
            fresh_interfaces = [
                path for path in interfaces
                if "MODULE Fresh" in path.read_text()
            ]
            self.assertEqual(len(fresh_interfaces), 1, interfaces)
            interface_text = fresh_interfaces[0].read_text()
            self.assertIn("MONAD-QTT-INTERFACE 12", interface_text)
            self.assertIn("payload=monad-hm-scheme-v1|", interface_text)
            self.assertIn(
                "payload=monad-callable-contract-v1|", interface_text)
            self.assertIn(
                "CONTRACT fresh-prefix parameters=1",
                interface_text,
            )
            self.assertIn(
                "RESULT type=k4; representation=2 mode=1 origin=3",
                interface_text,
            )
            executed = subprocess.run(
                [str(output)], cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "23")

            # Force the provider's object/sidecar path through the incremental
            # "object is newer than source" branch on filesystems with
            # one-second timestamp granularity.
            os.utime(directory / "Fresh.mon", (1, 1))
            cached_output = directory / "qtt-memory-imported-call-cached"
            cached = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(cached_output),
                    "--emit-llvm", "--trace=qtt-all",
                ],
                cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(cached.returncode, 0, cached.stdout)
            self.assertIn(
                "[qtt] imported verified interface contract "
                "Fresh.fresh-prefix",
                cached.stdout,
            )
            self.assertNotIn(
                "[qtt] import contract Fresh.fresh-prefix",
                cached.stdout,
            )
            cached_ir = program.with_suffix(".ll").read_text()
            cached_start = cached_ir.index(
                "define ptr @reclaim-imported")
            cached_end = cached_ir.index("\n}", cached_start)
            self.assertIn(
                "call void @free",
                cached_ir[cached_start:cached_end],
            )
            cached_run = subprocess.run(
                [str(cached_output)], cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(cached_run.returncode, 0, cached_run.stdout)
            self.assertEqual(cached_run.stdout.strip(), "23")

            stale_lines = fresh_interfaces[0].read_text().splitlines()
            stale_lines = [
                "ARTIFACT 0000000000000001"
                if line.startswith("ARTIFACT ") else line
                for line in stale_lines
            ]
            fresh_interfaces[0].write_text("\n".join(stale_lines) + "\n")
            fallback_output = (
                directory / "qtt-memory-imported-call-fallback")
            fallback = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(fallback_output),
                    "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(fallback.returncode, 0, fallback.stdout)
            self.assertNotIn(
                "Fresh.fresh-prefix as fresh-prefix",
                fallback.stdout,
            )
            fallback_ir = program.with_suffix(".ll").read_text()
            fallback_start = fallback_ir.index(
                "define ptr @reclaim-imported")
            fallback_end = fallback_ir.index("\n}", fallback_start)
            self.assertNotIn(
                "call void @free",
                fallback_ir[fallback_start:fallback_end],
            )
            fallback_run = subprocess.run(
                [str(fallback_output)], cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(fallback_run.returncode, 0, fallback_run.stdout)
            self.assertEqual(fallback_run.stdout.strip(), "23")

    def test_qualified_import_carries_provider_ownership_contract(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        provider = """
(module Fresh [fresh-prefix])
(define fresh-prefix
  (lambda ([text : String] -> String)
    (__rt_string_take text 3)))
"""
        consumer = """
(module Main)
(import qualified Fresh [fresh-prefix])
(define reclaim-qualified
  (lambda ()
    (with [text (Fresh.fresh-prefix "abcdef")]
      29)))
(show (reclaim-qualified))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "Fresh.mon").write_text(provider)
            program = directory / "Main.mon"
            output = directory / "qtt-memory-qualified-call"
            program.write_text(consumer)
            env = os.environ.copy()
            env["HOME"] = str(directory / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-all",
                ],
                cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index("define ptr @reclaim-qualified")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertIn("call ptr @Fresh__fresh-prefix", function)
            self.assertIn("call void @free", function)
            self.assertIn(
                "imported verified interface contract "
                "Fresh.fresh-prefix as Fresh.fresh-prefix",
                compiled.stdout,
            )
            executed = subprocess.run(
                [str(output)], cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "29")

    def test_imported_static_result_is_not_reclaimed(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        provider = """
(module Stable [stable-text])
(define stable-text
  (lambda ([ignored : Int] -> String)
    (if True "static storage" "static storage")))
"""
        consumer = """
(module Main)
(import Stable [stable-text])
(define keep-imported-static
  (lambda ()
    (with [text (stable-text 0)]
      31)))
(show (keep-imported-static))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "Stable.mon").write_text(provider)
            program = directory / "Main.mon"
            output = directory / "qtt-memory-imported-static"
            program.write_text(consumer)
            env = os.environ.copy()
            env["HOME"] = str(directory / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-all",
                ],
                cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index("define ptr @keep-imported-static")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertNotIn("call void @free", function)
            self.assertIn("result=borrowed/static", compiled.stdout)
            executed = subprocess.run(
                [str(output)], cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "31")

    def test_local_definition_shadows_imported_ownership_contract(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        provider = """
(module Stable [make-text])
(define make-text
  (lambda ([ignored : Int] -> String)
    (if True "provider static" "provider static")))
"""
        consumer = """
(module Main)
(import Stable [make-text])
(define make-text
  (lambda ([ignored : Int] -> String)
    (__rt_string_take "local fresh" 5)))
(define reclaim-shadow
  (lambda ()
    (with [text (make-text 0)]
      37)))
(show (reclaim-shadow))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "Stable.mon").write_text(provider)
            program = directory / "Main.mon"
            output = directory / "qtt-memory-shadow"
            program.write_text(consumer)
            env = os.environ.copy()
            env["HOME"] = str(directory / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(output), "--emit-llvm", "--trace=qtt-proof",
                ],
                cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ir = program.with_suffix(".ll").read_text()
            start = ir.index("define ptr @reclaim-shadow")
            end = ir.index("\n}", start)
            function = ir[start:end]
            self.assertIn("call ptr @make-text", function)
            self.assertIn("call void @free", function)
            executed = subprocess.run(
                [str(output)], cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "37")

    def test_disabled_or_moved_binding_does_not_emit_drop(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        cases = {
            "disabled": (
                {"MONAD_QTT_MEMORY": "0"},
                """
(define keep-static
  (lambda ()
    (with [text "not qtt owned"]
      7)))
(show (keep-static))
""",
            ),
            "analysis-disabled": (
                {"MONAD_QTT_ANALYSIS": "0"},
                """
(define keep-without-analysis
  (lambda ()
    (with [text "analysis disabled"]
      7)))
(show (keep-without-analysis))
""",
            ),
            "moved": (
                {},
                """
(define move-out
  (lambda ()
    (with [text "escapes"]
      text)))
(show (move-out))
""",
            ),
            "partially-moved": (
                {},
                """
(define move-one-out
  (lambda ()
    (with [dead "would be dead"
           escaping "escapes"]
      escaping)))
(show (move-one-out))
""",
            ),
        }
        for name, (extra_env, source) in cases.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                directory = Path(directory)
                program = directory / f"{name}.mon"
                output = directory / name
                program.write_text(source)
                env = os.environ.copy()
                env["HOME"] = str(directory)
                env.update(extra_env)
                compiled = subprocess.run(
                    [
                        str(resolve_monad_binary()), str(program),
                        "-o", str(output), "--emit-llvm",
                    ],
                    cwd=ROOT, env=env, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                )
                self.assertEqual(compiled.returncode, 0, compiled.stdout)
                ir = program.with_suffix(".ll").read_text()
                self.assertNotIn("call ptr @strdup", ir)
                self.assertNotIn("call void @free", ir)
                executed = subprocess.run(
                    [str(output)], cwd=ROOT, env=env, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                )
                self.assertEqual(executed.returncode, 0, executed.stdout)


if __name__ == "__main__":
    unittest.main()
