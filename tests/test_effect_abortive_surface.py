"""Real source-to-executable abortive handler slice."""

from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MASK64 = (1 << 64) - 1


def _mix64(value, component):
    return ((value ^ component) * 1099511628211) & MASK64


def _text_fingerprint(text):
    value = 1469598103934665603
    for byte in text.encode():
        value = _mix64(value, byte)
    return value or 1


def _callable_predicate_commitment(row, predicates):
    value = _mix64(1469598103934665603, row)
    value = _mix64(value, len(predicates))
    for stage, trait in predicates:
        value = _mix64(value, stage)
        for byte in trait.encode():
            value = _mix64(value, byte)
    return value or 1


def _judgment_commitment(row, constraints, result, predicates):
    value = _mix64(0x6566666563746A75, row)
    value = _mix64(value, constraints)
    value = _mix64(value, result)
    value = _mix64(value, len(predicates))
    for stage, trait in predicates:
        value = _mix64(value, stage)
        value = _mix64(value, _text_fingerprint(trait))
    return value or 1


class AbortiveSurfaceTests(unittest.TestCase):
    def test_abortive_handler_eliminates_a_strict_nested_context(self):
        program = r'''
(effect Demo.Nested.raise exception raise 0 False String Int)
(effect-handler Demo.Nested.catch Demo.Nested.raise 0 True)
define recover :: String -> Int
  message -> 9
tests
  assert-eq (handle Demo.Nested.catch
                    (+ 100 (+ 200 (perform Demo.Nested.raise "stop")))
                    recover) 9 "abort discards the strict evaluation context"
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "NestedAbortive.mon"
            executable = directory / "NestedAbortive"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source), "-o", str(executable)],
                cwd=ROOT, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)

    def test_nested_abortive_handler_rejects_multiple_matching_operations(self):
        program = r'''
(effect Demo.Ambiguous.raise exception raise 0 False Int Int)
(effect-handler Demo.Ambiguous.catch Demo.Ambiguous.raise 0 True)
define recover :: Int -> Int
  value -> value
define invalid
  handle Demo.Ambiguous.catch
         (+ (perform Demo.Ambiguous.raise 1)
            (perform Demo.Ambiguous.raise 2))
         recover
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "AmbiguousAbortive.mon"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source)], cwd=ROOT,
                env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn("multiple matching operations", compiled.stdout)

    def test_abortive_direct_lowering_rejects_residual_effectful_context(self):
        program = r'''
(effect Demo.Residual.raise exception raise 0 False Int Int)
(effect Demo.Residual.trace io trace 0 False Int Int)
(effect-handler Demo.Residual.catch Demo.Residual.raise 0 True)
define recover :: Int -> Int
  value -> value
define invalid
  handle Demo.Residual.catch
         (+ (perform Demo.Residual.trace 1)
            (perform Demo.Residual.raise 2))
         recover
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "ResidualAbortive.mon"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source)], cwd=ROOT,
                env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn("residual effectful context", compiled.stdout)

    def test_abortive_context_consumes_core_owned_latent_call_contract(self):
        program = r'''
(effect Demo.CoreLatent.raise exception raise 0 False Int Int)
(effect-handler Demo.CoreLatent.catch Demo.CoreLatent.raise 0 True)
define recover :: Int -> Int
  value -> value
define latent :: Int -io-> Int
  value -> value
define invalid
  handle Demo.CoreLatent.catch
         (+ (latent 1)
            (perform Demo.CoreLatent.raise 2))
         recover
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "CoreLatentAbortive.mon"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source)], cwd=ROOT,
                env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn("residual effectful context", compiled.stdout)

    def test_abortive_context_rejects_consumed_effect_trait_obligation(self):
        program = r'''
(effect Demo.Qualified.raise exception raise 0 False Int Int)
(effect-handler Demo.Qualified.catch Demo.Qualified.raise 0 True)
define recover :: Int -> Int
  value -> value
define qualified :: Int -e-> Int
  where e has io
  value -> value
define invalid
  handle Demo.Qualified.catch
         (+ (qualified 1)
            (perform Demo.Qualified.raise 2))
         recover
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "QualifiedAbortive.mon"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source)], cwd=ROOT,
                env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn("residual effectful context", compiled.stdout)

    def test_abortive_context_consumes_imported_latent_call_contract(self):
        provider_program = r'''
module LatentProvider [latent]
define latent :: Int -io-> Int
  value -> value
'''
        consumer_program = r'''
module LatentConsumer []
(import qualified LatentProvider [latent])
(effect LatentConsumer.raise exception raise 0 False Int Int)
(effect-handler LatentConsumer.catch LatentConsumer.raise 0 True)
define recover :: Int -> Int
  value -> value
define invalid
  handle LatentConsumer.catch
         (+ (LatentProvider.latent 1)
            (perform LatentConsumer.raise 2))
         recover
'''
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            provider = work / "LatentProvider.mon"
            consumer = work / "LatentConsumer.mon"
            provider.write_text(provider_program)
            consumer.write_text(consumer_program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            built_provider = subprocess.run(
                [str(ROOT / "monad"), str(provider), "-o",
                 str(work / "provider")], cwd=work, env=environment,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(
                built_provider.returncode, 0, built_provider.stdout)
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(consumer)], cwd=work,
                env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn("residual effectful context", compiled.stdout)

    def test_imported_qualified_effect_obligation_reaches_core(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            provider = work / "QualifiedProvider.mon"
            provider.write_text(r'''
module QualifiedProvider [qualified]
define qualified :: Int -e-> Int
  where e has io
  value -> value
''')
            consumer = work / "QualifiedConsumer.mon"
            consumer.write_text(r'''
module QualifiedConsumer []
(import qualified QualifiedProvider [qualified])
(effect QualifiedConsumer.raise exception raise 0 False Int Int)
(effect-handler QualifiedConsumer.catch QualifiedConsumer.raise 0 True)
define recover :: Int -> Int
  value -> value
define invalid
  handle QualifiedConsumer.catch
         (+ (QualifiedProvider.qualified 1)
            (perform QualifiedConsumer.raise 2))
         recover
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            built_provider = subprocess.run(
                [str(ROOT / "monad"), str(provider), "-o",
                 str(work / "provider")], cwd=work, env=environment,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(
                built_provider.returncode, 0, built_provider.stdout)
            interface_text = (work / "QualifiedProvider.mqti").read_text()
            self.assertIn("MONAD-QTT-INTERFACE 12", interface_text)
            self.assertRegex(
                interface_text,
                r"EFFECTJUDGMENT row=[0-9a-f]{16} "
                r"constraints=[0-9a-f]{16} result=1 count=1 .*"
                r"predicates=0:io")
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(consumer)], cwd=work,
                env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn("residual effectful context", compiled.stdout)

    def test_v12_rejects_self_consistent_judgment_forged_against_callable(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            provider = work / "QualifiedProvider.mon"
            provider.write_text(r'''
module QualifiedProvider [qualified]
define qualified :: Int -e-> Int
  where e has io
  value -> value
''')
            consumer = work / "QualifiedConsumer.mon"
            consumer.write_text(r'''
module QualifiedConsumer []
(import qualified QualifiedProvider [qualified])
define observed
  QualifiedProvider.qualified 1
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            built = subprocess.run(
                [str(ROOT / "monad"), str(provider), "-o",
                 str(work / "provider")], cwd=work, env=environment,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(built.returncode, 0, built.stdout)

            seeded = subprocess.run(
                [str(ROOT / "monad"), str(consumer), "--emit-obj"], cwd=work,
                env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(seeded.returncode, 0, seeded.stdout)

            interface = work / "QualifiedProvider.module.mqti"
            interface_times = interface.stat()
            text = interface.read_text()
            match = re.search(
                r"^EFFECTJUDGMENT row=([0-9a-f]{16}) .* result=(\d+) "
                r"count=1 .* predicates=0:io$", text, re.MULTILINE)
            self.assertIsNotNone(match, text)
            row = int(match.group(1), 16)
            result = int(match.group(2))
            predicates = [(0, "telemetry")]
            constraints = _callable_predicate_commitment(row, predicates)
            commitment = _judgment_commitment(
                row, constraints, result, predicates)
            forged = (
                f"EFFECTJUDGMENT row={row:016x} "
                f"constraints={constraints:016x} result={result} count=1 "
                f"commitment={commitment:016x} predicates=0:telemetry")
            text = re.sub(r"^EFFECTJUDGMENT .*$", forged, text,
                          flags=re.MULTILINE)
            interface.write_text(text)
            os.utime(interface, ns=(interface_times.st_atime_ns,
                                    interface_times.st_mtime_ns))
            provider_artifacts = sorted(
                path.name for path in work.glob("QualifiedProvider*"))
            object_paths = list(work.glob("QualifiedProvider*.o"))
            self.assertTrue(object_paths, provider_artifacts)
            object_time = object_paths[0].stat().st_mtime_ns
            os.utime(provider, ns=(object_time - 2_000_000_000,
                                   object_time - 2_000_000_000))

            forged_consumer = work / "ForgedConsumer.mon"
            forged_consumer.write_text(consumer.read_text().replace(
                "module QualifiedConsumer", "module ForgedConsumer"))
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(forged_consumer)], cwd=work,
                env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn(
                "incoherent imported effect judgment: "
                "QualifiedProvider.qualified", compiled.stdout)

    def test_unconsumed_qualified_stage_remains_latent(self):
        program = r'''
(effect Demo.Partial.raise exception raise 0 False Int Int)
(effect-handler Demo.Partial.catch Demo.Partial.raise 0 True)
define delayed :: Int -> Int -e-> Int
  where e has io
  x y -> (+ x y)
define recover :: Int -> Int
  value -> 17
tests
  assert-eq (handle Demo.Partial.catch
                    (begin (delayed 1)
                           (perform Demo.Partial.raise 2))
                    recover) 17 "a residual arrow keeps its future obligation"
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "PartialQualifiedAbortive.mon"
            executable = directory / "PartialQualifiedAbortive"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source), "-o", str(executable)],
                cwd=ROOT, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)

    def test_abortive_operation_in_branch_requires_branch_lowering(self):
        program = r'''
(effect Demo.Branch.raise exception raise 0 False Int Int)
(effect-handler Demo.Branch.catch Demo.Branch.raise 0 True)
define recover :: Int -> Int
  value -> value
define invalid
  handle Demo.Branch.catch
         (if True (perform Demo.Branch.raise 2) 3)
         recover
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "BranchAbortive.mon"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source)], cwd=ROOT,
                env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn("branch-sensitive lowering", compiled.stdout)

    def test_operation_scheme_identity_is_alpha_invariant(self):
        program = r'''
(effect Demo.Alpha.raise exception raise 0 False String a)
(effect Demo.Alpha.raise exception raise 0 False String b)
(effect-handler Demo.Alpha.catch Demo.Alpha.raise 0 True)
define recover :: String -> Int
  message -> 9
tests
  assert-eq (handle Demo.Alpha.catch
                    (perform Demo.Alpha.raise "x")
                    recover) 9 "renaming a binder preserves declaration identity"
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "AlphaOperation.mon"
            executable = directory / "AlphaOperation"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source), "-o", str(executable)],
                cwd=ROOT, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)

    def test_polymorphic_operation_scheme_is_fresh_at_each_handler(self):
        program = r'''
(effect Demo.Poly.raise exception raise 0 False String a)
(effect-handler Demo.Poly.catch Demo.Poly.raise 0 True)

define to-code :: String -> Int
  message -> 41
define to-message :: String -> String
  message -> "recovered"
define combined :: Int
  (+ (handle Demo.Poly.catch (perform Demo.Poly.raise "bad") to-code)
     (length (handle Demo.Poly.catch (perform Demo.Poly.raise "worse") to-message)))

tests
  assert-eq (handle Demo.Poly.catch
                    (perform Demo.Poly.raise "bad")
                    to-code) 41 "result variable instantiates to Int"
  assert-eq (handle Demo.Poly.catch
                    (perform Demo.Poly.raise "worse")
                    to-message) "recovered" "fresh instance becomes String"
  assert-eq combined 50 "two instances coexist in one inference context"
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "PolymorphicOperation.mon"
            executable = directory / "PolymorphicOperation"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source), "-o", str(executable)],
                cwd=ROOT, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)

    def test_core_owned_abortive_handler_compiles_and_runs(self):
        program = r'''
(effect Demo.Abort.raise exception raise 0 False Int String)
(effect-handler Demo.Abort.catch Demo.Abort.raise 0 True)

define explain :: Int -> String
  x -> "recovered"

tests
  assert-eq (handle Demo.Abort.catch
                    (perform Demo.Abort.raise 5)
                    explain) "recovered" "payload and result may differ"
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "AbortiveSurface.mon"
            executable = directory / "AbortiveSurface"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source), "-o", str(executable)],
                cwd=ROOT, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)

    def test_resumptive_profile_is_rejected_by_abortive_surface(self):
        program = r'''
(effect Demo.Once.read io read 1 False)
(effect-handler Demo.Once.handle Demo.Once.read 1 True)

define identity :: Int -> Int
  x -> x
define value
  handle Demo.Once.handle (perform Demo.Once.read 1) identity
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "UnsupportedHandler.mon"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source)], cwd=ROOT,
                env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn("one-shot handler lowering is not implemented",
                          compiled.stdout)

    def test_unhandled_perform_and_wrong_clause_fail_closed(self):
        unhandled = r'''
(effect Demo.Unhandled.raise exception raise 0 False)
define value
  perform Demo.Unhandled.raise 7
'''
        wrong_clause = r'''
(effect Demo.Typed.raise exception raise 0 False Int String)
(effect-handler Demo.Typed.catch Demo.Typed.raise 0 True)
define text-clause :: String -> String
  x -> x
define value
  handle Demo.Typed.catch (perform Demo.Typed.raise 7) text-clause
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            source = directory / "Unhandled.mon"
            source.write_text(unhandled)
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source)], cwd=ROOT,
                env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn("unhandled effect operation 'Demo.Unhandled.raise'",
                          compiled.stdout)

            source = directory / "WrongClause.mon"
            source.write_text(wrong_clause)
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source)], cwd=ROOT,
                env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(compiled.returncode, 0, compiled.stdout)
            self.assertIn("expected type 'String'", compiled.stdout)
            self.assertIn("actual type 'Int'", compiled.stdout)


if __name__ == "__main__":
    unittest.main()
