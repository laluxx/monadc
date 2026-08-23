import subprocess
import tempfile
import unittest
from pathlib import Path
import re
import os


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/embedding/branches/lifecycle/atom.embedding.01.lifecycle.opaque-runtime"
NATIVE_FIXTURE = ROOT / "tests/embedding/branches/native-call/atom.embedding.02.native-call.zero-wrapper"
BIDIRECTIONAL_FIXTURE = ROOT / "tests/embedding/branches/bidirectional/atom.embedding.03.bidirectional.zero-wrapper"
LIVE_FIXTURE = ROOT / "tests/embedding/branches/live-image/atom.embedding.04.live-image.function-cell"
SNAPSHOT_FIXTURE = ROOT / "tests/embedding/branches/live-image/atom.embedding.05.live-image.snapshot-values"
FOREIGN_FIXTURE = ROOT / "tests/embedding/branches/foreign-object/atom.embedding.06.foreign-object.nominal-lifetime"
EXECUTOR_FIXTURE = ROOT / "tests/embedding/branches/foreign-object/atom.embedding.07.foreign-object.executor-ownership"
TYPE_REFLECTION_FIXTURE = ROOT / "tests/embedding/branches/foreign-object/atom.embedding.08.foreign-object.generative-reflection"
COMPILER_IMPORT_FIXTURE = ROOT / "tests/embedding/branches/compiler-session/atom.embedding.10.compiler-session.transactional-foreign-import"
INSTALLED_FIXTURE = ROOT / "tests/embedding/branches/installed-sdk/atom.embedding.13.installed-sdk.c-cpp-static-shared"
SOURCE_FIXTURE = ROOT / "tests/embedding/branches/source-unit/atom.embedding.15.source-unit.owned-versioned-buffer"
DIAGNOSTIC_FIXTURE = ROOT / "tests/embedding/branches/diagnostics/atom.embedding.16.diagnostics.immutable-input-check"


class EmbeddingApiTests(unittest.TestCase):
    def test_embedding_gate_covers_every_embedding_proof_layer(self):
        makefile = (ROOT / "Makefile").read_text()
        match = re.search(r"^test-embedding:.*?(?=^\S|\Z)", makefile,
                          re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(match, makefile)
        recipe = match.group(0)
        for module in (
            "tests.test_embedding",
            "tests.test_qtt_foreign_call",
            "tests.test_qtt_foreign_lowering",
            "tests.test_qtt_foreign_llvm",
            "tests.test_embedding_foreign_execution",
            "tests.test_embedding_source_unit",
            "tests.test_embedding_diagnostics",
            "tests.test_embedding_parser_context",
            "tests.test_embedding_parser_unwind",
            "tests.test_embedding_frontend_capsule",
            "tests.test_embedding_ast_transaction",
            "tests.test_embedding_frontend_transaction",
            "tests.test_embedding_public_parse_diagnostics",
            "tests.test_embedding_frontend_parallel",
            "tests.test_embedding_frontend_session_state",
            "tests.test_embedding_frontend_registry_state",
            "tests.test_embedding_frontend_type_state",
            "tests.test_embedding_compilation_unit",
            "tests.test_embedding_type_diagnostics",
            "tests.test_embedding_inference_transaction",
            "tests.test_embedding_environment_snapshot",
            "tests.test_embedding_environment_queries",
            "tests.test_embedding_surface_source",
            "tests.test_embedding_source_hooks",
            "tests.test_embedding_native_string",
            "tests.test_embedding_atomic_redefinition",
            "tests.test_embedding_read_generation",
            "tests.test_embedding_orc_reclamation",
            "tests.test_embedding_releasable_function",
            "tests.test_embedding_qtt_report",
            "tests.test_embedding_qtt_quantities",
            "tests.test_embedding_resource_certificate",
            "tests.test_embedding_how_to",
        ):
            self.assertIn(module, recipe)

    def test_embedding_c_fixtures_have_living_metadata(self):
        fixtures = list((ROOT / "tests/embedding").rglob("host.c"))
        fixtures += list((ROOT / "tests/embedding").rglob("host.cpp"))
        fixtures += [
            ROOT / "tests/qtt/branches/foreign-call/atom.qtt.09.foreign-call.nominal-transfer/host.c",
            ROOT / "tests/qtt/branches/foreign-lowering/atom.qtt.11.foreign-lowering.exact-runtime-actions/host.c",
            ROOT / "tests/qtt/branches/foreign-llvm/atom.qtt.12.foreign-llvm.verified-direct-calls/host.c",
        ]
        self.assertGreaterEqual(len(fixtures), 13)
        ids = set()
        required = ("TEST-ID", "TEST-CONTEXT", "TEST-PURPOSE", "TEST-ATOM",
                    "TEST-EXPECT", "TEST-MENU-PATH")
        for fixture in fixtures:
            source = fixture.read_text()
            for key in required:
                self.assertRegex(source, rf"(?m)^:{key} .+$", f"{fixture}: {key}")
            test_id = re.search(r"(?m)^:TEST-ID (.+)$", source).group(1)
            self.assertNotIn(test_id, ids, f"duplicate TEST-ID: {test_id}")
            ids.add(test_id)

    def test_embedding_is_a_self_documenting_subsystem(self):
        self.assertTrue((ROOT / "embed/README.org").is_file())
        self.assertTrue((ROOT / "embed/embed.c").is_file())
        self.assertTrue((ROOT / "embed/include/monad/embed.h").is_file())

    def test_make_install_includes_embedding_sdk(self):
        with tempfile.TemporaryDirectory(prefix="monadc-embed-install-") as directory:
            result = subprocess.run(
                ["make", "install", f"PREFIX={directory}"], cwd=ROOT,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            prefix = Path(directory)
            self.assertTrue((prefix / "include/monad/embed.h").is_file())
            self.assertTrue((prefix / "include/monad/monad.h").is_file())
            self.assertTrue((prefix / "include/monad/qtt.h").is_file())
            self.assertTrue((prefix / "lib/libmonad-embed.a").is_file())
            self.assertTrue((prefix / "lib/libmonad-embed.so").is_file())
            self.assertTrue((prefix / "include/monad/compiler.h").is_file())
            self.assertTrue((prefix / "lib/libmonad-compiler.a").is_file())
            self.assertTrue((prefix / "lib/libmonad-compiler.so").is_file())

            programs = []
            for linkage, libraries in (
                ("static", [str(prefix / "lib/libmonad-embed.a")]),
                ("shared", ["-L", str(prefix / "lib"), "-lmonad-embed",
                            f"-Wl,-rpath,{prefix / 'lib'}"]),
            ):
                c_program = prefix / f"lifecycle-{linkage}"
                result = subprocess.run(
                    ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                     "-I", str(prefix / "include"), str(FIXTURE / "host.c"),
                     *libraries, "-pthread", "-o", str(c_program)],
                    cwd=prefix, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, check=False)
                self.assertEqual(result.returncode, 0, result.stdout)
                programs.append((c_program, (FIXTURE / "stdout").read_text()))

                cpp_program = prefix / f"cpp-{linkage}"
                result = subprocess.run(
                    ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                     "-I", str(prefix / "include"),
                     str(INSTALLED_FIXTURE / "host.cpp"), *libraries,
                     "-pthread", "-o", str(cpp_program)], cwd=prefix, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
                self.assertEqual(result.returncode, 0, result.stdout)
                programs.append((cpp_program,
                    (INSTALLED_FIXTURE / "stdout").read_text()))

            for linkage, libraries in (
                ("static", [str(prefix / "lib/libmonad-compiler.a"),
                            str(prefix / "lib/libmonad-embed.a")]),
                ("shared", ["-L", str(prefix / "lib"), "-lmonad-compiler",
                            "-lmonad-embed", f"-Wl,-rpath,{prefix / 'lib'}"]),
            ):
                compiler_program = prefix / f"compiler-{linkage}"
                result = subprocess.run(
                    ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                     "-I", str(prefix / "include"),
                     str(COMPILER_IMPORT_FIXTURE / "host.c"), *libraries,
                     "-pthread", "-o", str(compiler_program)], cwd=prefix,
                    text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, check=False)
                self.assertEqual(result.returncode, 0, result.stdout)
                programs.append((compiler_program,
                    (COMPILER_IMPORT_FIXTURE / "stdout").read_text()))

                source_program = prefix / f"source-{linkage}"
                result = subprocess.run(
                    ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                     "-I", str(prefix / "include"),
                     str(SOURCE_FIXTURE / "host.c"), *libraries,
                     "-pthread", "-o", str(source_program)], cwd=prefix,
                    text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, check=False)
                self.assertEqual(result.returncode, 0, result.stdout)
                programs.append((source_program,
                    (SOURCE_FIXTURE / "stdout").read_text()))

                diagnostic_program = prefix / f"diagnostics-{linkage}"
                result = subprocess.run(
                    ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                     "-I", str(prefix / "include"),
                     str(DIAGNOSTIC_FIXTURE / "host.c"), *libraries,
                     "-pthread", "-o", str(diagnostic_program)], cwd=prefix,
                    text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, check=False)
                self.assertEqual(result.returncode, 0, result.stdout)
                programs.append((diagnostic_program,
                    (DIAGNOSTIC_FIXTURE / "stdout").read_text()))

            for program, expected in programs:
                result = subprocess.run([str(program)], cwd=prefix, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertEqual(result.stdout, expected)

    def test_opaque_runtime_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="monadc-embed-") as directory:
            executable = Path(directory) / "host"
            compile_result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "embed/include"), str(FIXTURE / "host.c"),
                 str(ROOT / "libmonad-embed.a"), "-pthread", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            run_result = subprocess.run(
                [str(executable)], text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(run_result.returncode, 0, run_result.stdout)
            self.assertEqual(run_result.stdout, (FIXTURE / "stdout").read_text())

    def test_runtime_lifecycle_under_address_and_undefined_sanitizers(self):
        # Compiler-rt sanitizer contracts:
        # https://clang.llvm.org/docs/AddressSanitizer.html
        # https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html
        with tempfile.TemporaryDirectory(prefix="monadc-embed-sanitize-") as directory:
            executable = Path(directory) / "host"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g",
                 "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                 "-I", str(ROOT / "embed/include"), str(FIXTURE / "host.c"),
                 str(ROOT / "embed/embed.c"), "-pthread", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            environment = os.environ.copy()
            environment["ASAN_OPTIONS"] = "detect_leaks=0"
            result = subprocess.run([str(executable)], cwd=ROOT, text=True,
                env=environment,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, (FIXTURE / "stdout").read_text())

    def test_native_call_is_the_exact_implementation_pointer(self):
        with tempfile.TemporaryDirectory(prefix="monadc-native-embed-") as directory:
            executable = Path(directory) / "host"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "embed/include"), str(NATIVE_FIXTURE / "host.c"),
                 str(ROOT / "libmonad-embed.a"), "-pthread", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, (NATIVE_FIXTURE / "stdout").read_text())

    def test_native_abi_uses_structural_signatures(self):
        header = (ROOT / "embed/include/monad/embed.h").read_text()
        self.assertIn("monad_abi_signature_equal", header)
        self.assertNotIn("expected_signature_fingerprint", header)

    def test_bidirectional_native_calls_are_exact_addresses(self):
        with tempfile.TemporaryDirectory(prefix="monadc-bidirectional-") as directory:
            executable = Path(directory) / "host"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "embed/include"),
                 str(BIDIRECTIONAL_FIXTURE / "host.c"),
                 str(ROOT / "libmonad-embed.a"), "-pthread", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout,
                (BIDIRECTIONAL_FIXTURE / "stdout").read_text())

    def test_live_function_cell_redefinition(self):
        with tempfile.TemporaryDirectory(prefix="monadc-live-image-") as directory:
            executable = Path(directory) / "host"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "embed/include"), str(LIVE_FIXTURE / "host.c"),
                 str(ROOT / "libmonad-embed.a"), "-pthread", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, (LIVE_FIXTURE / "stdout").read_text())

    def test_immutable_typed_binding_snapshots(self):
        with tempfile.TemporaryDirectory(prefix="monadc-binding-snapshot-") as directory:
            executable = Path(directory) / "host"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "embed/include"),
                 str(SNAPSHOT_FIXTURE / "host.c"),
                 str(ROOT / "libmonad-embed.a"), "-pthread", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, (SNAPSHOT_FIXTURE / "stdout").read_text())

    def test_nominal_foreign_object_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="monadc-foreign-object-") as directory:
            executable = Path(directory) / "host"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "embed/include"),
                 str(FOREIGN_FIXTURE / "host.c"),
                 str(ROOT / "libmonad-embed.a"), "-pthread", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, (FOREIGN_FIXTURE / "stdout").read_text())

    def test_foreign_object_executor_and_unique_policy(self):
        with tempfile.TemporaryDirectory(prefix="monadc-foreign-executor-") as directory:
            executable = Path(directory) / "host"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "embed/include"),
                 str(EXECUTOR_FIXTURE / "host.c"),
                 str(ROOT / "libmonad-embed.a"), "-pthread", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, (EXECUTOR_FIXTURE / "stdout").read_text())

    def test_foreign_type_generative_reflection(self):
        with tempfile.TemporaryDirectory(prefix="monadc-foreign-reflection-") as directory:
            executable = Path(directory) / "host"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "embed/include"),
                 str(TYPE_REFLECTION_FIXTURE / "host.c"),
                 str(ROOT / "libmonad-embed.a"), "-pthread", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout,
                (TYPE_REFLECTION_FIXTURE / "stdout").read_text())

    def test_compiler_session_transactional_foreign_import(self):
        with tempfile.TemporaryDirectory(prefix="monadc-compiler-session-") as directory:
            executable = Path(directory) / "host"
            result = subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "embed/include"),
                 str(COMPILER_IMPORT_FIXTURE / "host.c"),
                 str(ROOT / "libmonad-compiler.a"),
                 str(ROOT / "libmonad-embed.a"), "-pthread", "-o", str(executable)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = subprocess.run([str(executable)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout,
                (COMPILER_IMPORT_FIXTURE / "stdout").read_text())


if __name__ == "__main__":
    unittest.main()
