"""Public reader and Wisp effect-arrow labels survive portable HM export."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from src.testing.monad_binary import resolve_monad_binary
ROOT = Path(__file__).resolve().parents[3]


class EffectArrowSyntaxTests(unittest.TestCase):
    def test_guarded_mutation_cannot_hide_state_write(self):
        """Guard lowering must preserve the same effect as a plain clause."""
        bodies = {
            "plain": "index -> cells[index] <- 1",
            "guarded": "index | otherwise -> cells[index] <- 1",
        }
        for label, body in bodies.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                source = work / f"{label.title()}Mutation.mon"
                source.write_text(
                    "module Mutation []\n"
                    "define cells :: [8 Int]\n"
                    "  []\n\n"
                    "define write-cell :: Int -> Int\n"
                    f"  {body}\n"
                )
                env = os.environ.copy()
                env["MONAD_CORE"] = str(ROOT / "core")
                env["HOME"] = str(work / "home")
                Path(env["HOME"]).mkdir()
                compiled = subprocess.run(
                    [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                    cwd=work, env=env, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                )
                self.assertNotEqual(compiled.returncode, 0)
                self.assertIn("Missing effect annotation", compiled.stderr)
                self.assertIn("state.write", compiled.stderr)

                source.write_text(source.read_text().replace(
                    "define write-cell :: Int -> Int",
                    "define write-cell :: Int -state.writee-> Int",
                ))
                compiled = subprocess.run(
                    [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                    cwd=work, env=env, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                )
                self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_declared_trait_label_is_single_constraint_shorthand(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "TraitArrowShorthand.mon"
            source.write_text("""
module TraitArrowShorthand [qualified]
define qualified :: Int -io-> Int
  value -> value
""")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(ROOT / "core")
            cache_home = work / "home"
            cache_home.mkdir()
            env["HOME"] = str(cache_home)
            compiled = subprocess.run(
                [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                cwd=work, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            interface = source.with_suffix(".mqti").read_text()
            self.assertIn("predicates=0:io\n", interface)
            self.assertIn("|1|0:io", interface)

    def test_local_nominal_inside_applied_effect_result_is_portable(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "NominalEffectResult.mon"
            source.write_text("""
import Data.Either
module NominalEffectResult [qualified]
data LocalFailure = LocalFailure Int
define qualified :: Int -e-> Either LocalFailure Int
  where e has io
  value -> Right value
""")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(ROOT / "core")
            cache_home = work / "home"
            cache_home.mkdir()
            env["HOME"] = str(cache_home)
            compiled = subprocess.run(
                [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                cwd=work, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            interface = source.with_suffix(".mqti").read_text()
            self.assertRegex(interface, r"(?m)^CONTRACT qualified ")
            self.assertNotIn("k29;", interface)  # TYPE_UNKNOWN
            self.assertIn("|0:io", interface)

    def test_core_owned_handler_profiles_validate_resumption_bounds(self):
        valid = """
module HandlerProfile []
(effect HandlerProfile.raise exception raise 0 False)
(effect HandlerProfile.tick state tick 1 False)
(effect-handler HandlerProfile.abort HandlerProfile.raise 0 True)
(effect-handler HandlerProfile.tick-once HandlerProfile.tick 1 True)
"""
        invalid_cases = {
            "unknown handled effect": """
module UnknownHandlerProfile []
(effect-handler UnknownHandlerProfile.bad Missing.effect 0 True)
""",
            "exceeds operation resumption": """
module ExcessiveHandlerProfile []
(effect ExcessiveHandlerProfile.tick state tick 1 False)
(effect-handler ExcessiveHandlerProfile.bad ExcessiveHandlerProfile.tick 2 True)
""",
            "deep flag must be true or false": """
module BadDepthHandlerProfile []
(effect BadDepthHandlerProfile.tick state tick 1 False)
(effect-handler BadDepthHandlerProfile.bad BadDepthHandlerProfile.tick 1 Maybe)
""",
            "conflicting effect-handler profile": """
module ConflictingHandlerProfile []
(effect ConflictingHandlerProfile.tick state tick 1 False)
(effect-handler ConflictingHandlerProfile.same ConflictingHandlerProfile.tick 0 True)
(effect-handler ConflictingHandlerProfile.same ConflictingHandlerProfile.tick 1 True)
""",
        }
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            env = os.environ.copy()
            env["MONAD_CORE"] = str(ROOT / "core")
            source = work / "HandlerProfile.mon"
            source.write_text(valid)
            subprocess.run(
                [str(resolve_monad_binary()), str(source), "-o", str(work / "valid")],
                cwd=work, env=env, check=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            profile_interface = source.with_suffix(".mqti").read_text()
            self.assertIn("HANDLERS 2", profile_interface)
            self.assertIn(
                "HANDLERPROFILE name=HandlerProfile.abort", profile_interface)
            self.assertIn(
                "HANDLERPROFILE name=HandlerProfile.tick-once", profile_interface)
            for index, (expected, program) in enumerate(invalid_cases.items()):
                with self.subTest(expected=expected):
                    source = work / f"BadHandlerProfile{index}.mon"
                    source.write_text(program)
                    compiled = subprocess.run(
                        [str(resolve_monad_binary()), str(source),
                         "-o", str(work / f"bad{index}")],
                        cwd=work, env=env,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        text=True,
                    )
                    self.assertNotEqual(compiled.returncode, 0)
                    self.assertIn(expected, compiled.stderr.lower())

    def test_indented_wisp_effect_qualification_reaches_portable_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "IndentedQualification.mon"
            source.write_text("""
module IndentedQualification [qualified]

define qualified :: Int -e-> Int
  where e has io
  x -> x
""")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(ROOT / "core")
            subprocess.run(
                [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                cwd=work, env=env, check=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            interface = source.with_suffix(".mqti").read_text()
            self.assertIn("monad-callable-contract-v2|", interface)
            self.assertIn("|0:io", interface)

    def test_effect_qualification_order_has_canonical_interface_identity(self):
        payloads = []
        for qualification in (
            "e has state, e has io and e has telemetry",
            "e has telemetry, e has state and e has io",
        ):
            with tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                source = work / "CanonicalQualification.mon"
                source.write_text(f"""
module CanonicalQualification [qualified]

define qualified :: Int -e-> Int where {qualification}
  x -> x
""")
                env = os.environ.copy()
                env["MONAD_CORE"] = str(ROOT / "core")
                subprocess.run(
                    [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                    cwd=work, env=env, check=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                )
                contracts = [
                    line.partition(" payload=")[2]
                    for line in source.with_suffix(".mqti").read_text().splitlines()
                    if line.startswith("CONTRACT ") and " payload=" in line
                ]
                payloads.append(contracts)
        self.assertEqual(payloads[0], payloads[1])

    def test_imported_effect_qualification_is_reexported_losslessly(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            provider = work / "EffectProvider.mon"
            provider.write_text("""
module EffectProvider [qualified]

define qualified :: Int -e-> Int
  where e has io
  x -> x
""")
            consumer = work / "EffectConsumer.mon"
            consumer.write_text("""
module EffectConsumer [through]
(import qualified EffectProvider [qualified])

define through :: Int -e-> Int
  where e has io
  x -> EffectProvider.qualified x
""")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(ROOT / "core")
            for source in (provider, consumer):
                subprocess.run(
                    [str(resolve_monad_binary()), str(source), "-o", str(work / source.stem)],
                    cwd=work, env=env, check=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                )

            provider_contracts = [
                line.partition(" payload=")[2]
                for line in provider.with_suffix(".mqti").read_text().splitlines()
                if (line.startswith("HM ") or line.startswith("EFFECT "))
                and " payload=" in line
            ]
            consumer_contracts = [
                line.partition(" payload=")[2]
                for line in consumer.with_suffix(".mqti").read_text().splitlines()
                if (line.startswith("HM ") or line.startswith("EFFECT "))
                and " payload=" in line
            ]
            self.assertEqual(provider_contracts[0], consumer_contracts[0])
            self.assertTrue(all(item.endswith("|0:io") for item in provider_contracts))
            self.assertTrue(all(item.endswith("|0:io") for item in consumer_contracts))
            self.assertTrue(any("monad-hm-scheme-v2|" in item for item in consumer_contracts))
            self.assertTrue(any("monad-callable-contract-v2|" in item for item in consumer_contracts))

    def test_source_free_import_replays_certified_effect_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            provider = work / "BinaryEffectProvider.mon"
            provider.write_text("""
module BinaryEffectProvider [qualified]
(effect BinaryEffectProvider.tick state tick 0 False String a)
(effect-handler BinaryEffectProvider.tick-once BinaryEffectProvider.tick 0 True)
define qualified :: Int -e-> Int where e has io
  x -> x
""")
            bootstrap = work / "Bootstrap.mon"
            bootstrap.write_text("""
module Bootstrap []
(import qualified BinaryEffectProvider [qualified])
define bootstrap :: Int -e-> Int where e has io
  x -> BinaryEffectProvider.qualified x
""")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(ROOT / "core")
            subprocess.run(
                [str(resolve_monad_binary()), str(bootstrap), "--emit-obj",
                 "-o", str(work / "bootstrap")],
                cwd=work, env=env, check=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertTrue((work / "BinaryEffectProvider.module.o").exists())
            self.assertTrue((work / "BinaryEffectProvider.module.mqti").exists())
            provider_metadata = (
                work / "BinaryEffectProvider.module.mqti"
            ).read_text()
            self.assertIn("HANDLERS 1", provider_metadata)
            self.assertIn("BinaryEffectProvider.tick-once", provider_metadata)
            self.assertIn("payload=String result=a scheme=monad-hm-scheme-v1|",
                          provider_metadata)
            provider.rename(work / "BinaryEffectProvider.source")

            consumer = work / "SourceFreeConsumer.mon"
            consumer.write_text("""
module SourceFreeConsumer [through]
(import qualified BinaryEffectProvider [qualified])
define to-code :: String -> Int
  x -> 7
define to-message :: String -> String
  x -> "source-free"
define through :: Int -e-> Int where e has io
  x -> BinaryEffectProvider.qualified x
tests
  assert-eq (handle BinaryEffectProvider.tick-once
                    (perform BinaryEffectProvider.tick "one")
                    to-code) 7 "polymorphic source-free result"
  assert-eq (handle BinaryEffectProvider.tick-once
                    (perform BinaryEffectProvider.tick "two")
                    to-message) "source-free" "fresh source-free instance"
""")
            subprocess.run(
                [str(resolve_monad_binary()), str(consumer), "--emit-obj",
                 "-o", str(work / "consumer")],
                cwd=work, env=env, check=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            subprocess.run(
                [str(work / "consumer")], cwd=work, env=env, check=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            interface = consumer.with_suffix(".mqti").read_text()
            self.assertIn("monad-hm-scheme-v2|", interface)
            self.assertIn("monad-callable-contract-v2|", interface)
            self.assertIn("|0:io", interface)

            provider_interface = work / "BinaryEffectProvider.module.mqti"
            tampered = provider_interface.read_text()
            provider_interface.write_text(
                tampered.replace("|0:io", "|0:ix", 1)
            )
            rejected = subprocess.run(
                [str(resolve_monad_binary()), str(consumer), "--emit-obj",
                 "-o", str(work / "rejected")],
                cwd=work, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertNotEqual(rejected.returncode, 0)
            diagnostic = (rejected.stdout + rejected.stderr).lower()
            self.assertIn("certified binary interface rejected", diagnostic)

            provider_interface.write_text(tampered)
            provider_object = work / "BinaryEffectProvider.module.o"
            original_object = provider_object.read_bytes()
            provider_object.write_bytes(original_object + b"\0")
            object_rejected = subprocess.run(
                [str(resolve_monad_binary()), str(consumer), "--emit-obj",
                 "-o", str(work / "object-rejected")],
                cwd=work, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertNotEqual(object_rejected.returncode, 0)
            object_diagnostic = (
                object_rejected.stdout + object_rejected.stderr
            ).lower()
            self.assertIn("object binding mismatch", object_diagnostic)


    def test_wisp_multistage_labels_reach_interface_type_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "EffectArrowPortable.mon"
            source.write_text("""
module EffectArrowPortable [stages]

define stages :: Int -e-> Int -f-> Int
  x y -> 11
""")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(ROOT / "core")
            subprocess.run(
                [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                cwd=work, env=env, check=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            interface = source.with_suffix(".mqti").read_text()
            self.assertIn("CONTRACT stages parameters=2", interface)
            # QTT type encoding uses hexadecimal label payloads after '@'.
            self.assertIn("@65;", interface)  # e
            self.assertIn("@66;", interface)  # f

    def test_definition_rejects_inconsistent_concrete_effect_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "BadEffectContract.mon"
            source.write_text("""
module BadEffectContract [bad]

define bad :: Int -io.read-> Int
  x -> 11
""")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                cwd=work, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertNotEqual(compiled.returncode, 0)
            self.assertIn("effect contract", compiled.stderr.lower())

    def test_definition_requires_inferred_effect_on_plain_arrow(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "MissingEffectAnnotation.mon"
            source.write_text("""
module MissingEffectAnnotation [raise-value]

define latent :: Int -io-> Int
  value -> value

define raise-value :: Int -> Int
  value -> latent value
""")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(ROOT / "core")
            compiled = subprocess.run(
                [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                cwd=work, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertNotEqual(compiled.returncode, 0)
            self.assertIn("missing effect annotation", compiled.stderr.lower())
            self.assertIn(
                "Int -e-> Int", compiled.stderr)
            source.write_text(source.read_text().replace(
                "raise-value :: Int -> Int",
                "raise-value :: Int -e-> Int"))
            corrected = subprocess.run(
                [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                cwd=work, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(corrected.returncode, 0, corrected.stderr)

            source.write_text(source.read_text().replace(
                "raise-value :: Int -e-> Int",
                "raise-value :: Int -> Int"))
            permissive = subprocess.run(
                [str(resolve_monad_binary()), str(source),
                 "--allow-implicit-effects", "-o", str(work / "permissive")],
                cwd=work, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(permissive.returncode, 0, permissive.stderr)

    def test_wisp_effect_qualification_reaches_portable_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "QualifiedEffect.mon"
            source.write_text("""
module QualifiedEffect [qualified]

define qualified :: Int -e-> Int where e has io
  x -> x
""")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(ROOT / "core")
            subprocess.run(
                [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                cwd=work, env=env, check=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            interface = source.with_suffix(".mqti").read_text()
            self.assertIn("monad-hm-scheme-v2|", interface)
            self.assertIn("monad-callable-contract-v2|", interface)
            self.assertIn("0:io", interface)

    def test_effect_qualification_diagnostics_are_definition_local(self):
        cases = {
            "missing row binder": """
module MissingEffectRow []
define bad :: Int -e-> Int where f has io
  x -> x
""",
            "does not satisfy trait": """
module UnsatisfiedEffectTrait []
define bad :: Int -io.read-> Int where io.read has state
  x -> x
""",
        }
        for expected, text in cases.items():
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                source = work / "BadQualification.mon"
                source.write_text(text)
                env = os.environ.copy()
                env["MONAD_CORE"] = str(ROOT / "core")
                compiled = subprocess.run(
                    [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                    cwd=work, env=env,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                )
                self.assertNotEqual(compiled.returncode, 0)
                self.assertIn(expected, compiled.stderr.lower())

    def test_multiple_qualifications_are_canonical_and_portable(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "MultipleQualifications.mon"
            source.write_text("""
module MultipleQualifications [qualified]

define qualified :: Int -e-> Int where e has state, e has io and e has telemetry
  x -> x
""")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(ROOT / "core")
            subprocess.run(
                [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                cwd=work, env=env, check=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            interface = source.with_suffix(".mqti").read_text()
            self.assertIn("|0:io,0:state,0:telemetry", interface)

    def test_ordinary_reader_qualification_matches_wisp(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "ReaderQualification.mon"
            source.write_text("""
module ReaderQualification [qualified]

(define (qualified Int -e-> Int where e has io) 7)
""")
            env = os.environ.copy()
            env["MONAD_CORE"] = str(ROOT / "core")
            subprocess.run(
                [str(resolve_monad_binary()), str(source), "-o", str(work / "out")],
                cwd=work, env=env, check=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            interface = source.with_suffix(".mqti").read_text()
            self.assertIn("monad-callable-contract-v2|", interface)
            self.assertIn("|0:io", interface)


if __name__ == "__main__":
    unittest.main()
