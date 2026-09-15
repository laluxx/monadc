"""Compiler-wide QTT integration policy and stable module identity."""

from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest


from src.testing.monad_binary import resolve_monad_binary
ROOT = Path(__file__).resolve().parents[3]


class QttCompilerPolicyTests(unittest.TestCase):
    def test_stored_unique_closure_reclaims_result_and_environment(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(module QttStoredClosure)
(define stored-discard
  (lambda (-> Int)
    (with [payload "stored capture"]
      (with [f (lambda ([unit : Int] -> String)
                  (begin unit payload))]
        (with [ignored (f 0)]
          66)))))
(show (stored-discard))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttStoredClosure.mon"
            output = directory / "qtt-stored-closure"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [str(resolve_monad_binary()), str(program), "-o", str(output),
                 "--emit-llvm", "--trace=qtt-proof"],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            trace = compiled.stdout[compiled.stdout.index(
                "definition stored-discard"):]
            self.assertIn("typed Core: valid", trace)
            self.assertIn("ownership ANF: valid", trace)
            self.assertIn("verdict: VERIFIED", trace)
            self.assertRegex(trace, r"op i\d+: drop  κ#\d+")
            self.assertRegex(
                trace,
                r"lifetime κ#\d+ materialization=unique-closure destroy=yes",
            )
            self.assertRegex(
                trace,
                r"reclaim f#\d+ materialization=unique-closure moved-mask=#1",
            )
            ir = program.with_suffix(".ll").read_text()
            body = ir[ir.index("define i64 @stored-discard"):]
            body = body[:body.index("\n}")]
            self.assertEqual(body.count("call ptr @strdup"), 1, body)
            self.assertEqual(body.count("call void @free"), 1, body)
            self.assertEqual(
                body.count("call ptr @rt_value_string_take"), 1, body)
            self.assertNotIn("call ptr @rt_value_string(", body)
            self.assertEqual(
                body.count("call void @rt_closure_destroy_unique"), 1,
                body,
            )
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "66")

    def test_unique_capturing_closure_moves_and_reclaims_string(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(module QttCapturingClosure)
(define capture-discard
  (lambda (-> Int)
    (with [payload "captured"]
      (with [ignored
              ((lambda ([unit : Int] -> String) (begin unit payload)) 0)]
        65))))
(show (capture-discard))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttCapturingClosure.mon"
            output = directory / "qtt-capturing-closure"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [str(resolve_monad_binary()), str(program), "-o", str(output),
                 "--emit-llvm", "--trace=qtt-proof"],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            trace = compiled.stdout[compiled.stdout.index(
                "definition capture-discard"):]
            self.assertIn("typed Core: valid", trace)
            self.assertIn("ownership ANF: valid", trace)
            self.assertIn("verdict: VERIFIED", trace)
            self.assertRegex(trace, r"op i\d+: drop  κ#\d+")
            ir = program.with_suffix(".ll").read_text()
            body = ir[ir.index("define i64 @capture-discard"):]
            body = body[:body.index("\n}")]
            self.assertEqual(body.count("call ptr @strdup"), 1, body)
            self.assertEqual(body.count("call void @free"), 1, body)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "65")

    def test_discarded_lexical_call_result_is_materialized_freed_and_executes(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(module QttLexicalDiscard)
(define discard
  (lambda (-> Int)
    (with [f (lambda ([value : String] -> String) value)]
      (with [g f]
        (with [payload "discard me"]
          (with [ignored (g payload)]
            64))))))
(show (discard))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttLexicalDiscard.mon"
            output = directory / "qtt-lexical-discard"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [str(resolve_monad_binary()), str(program), "-o", str(output),
                 "--emit-llvm", "--trace=qtt-proof"],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            discard_trace = compiled.stdout[compiled.stdout.index(
                "definition discard"):]
            self.assertIn("typed Core: valid", discard_trace)
            self.assertIn("ownership ANF: valid", discard_trace)
            self.assertIn("verdict: VERIFIED", discard_trace)
            self.assertRegex(discard_trace, r"op i\d+: drop  κ#\d+")
            ir = program.with_suffix(".ll").read_text()
            discard_ir = ir[ir.index("define i64 @discard"):]
            discard_ir = discard_ir[:discard_ir.index("\n}")]
            self.assertIn("call ptr @strdup", discard_ir)
            self.assertEqual(discard_ir.count("call ptr @strdup"), 1,
                             discard_ir)
            self.assertEqual(discard_ir.count("call void @free"), 1,
                             discard_ir)
            self.assertIn("ret i64 64", discard_ir)
            self.assertRegex(
                compiled.stdout,
                r"\[qtt\] reclaim ignored#\d+ .*destructor=free",
            )
            self.assertRegex(
                compiled.stdout,
                r"\[qtt\] lifetime κ#\d+ materialization=static-copy "
                r"destroy=no",
            )
            self.assertRegex(
                compiled.stdout,
                r"\[qtt\] lifetime κ#\d+ materialization=owned-result "
                r"destroy=yes",
            )
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "64")

    def test_lexical_callable_is_verified_and_executes_from_source(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(module QttLexicalCallable)
(define run
  (lambda (-> String)
    (with [f (lambda ([value : String] -> String) value)]
      (with [g f]
        (with [payload "owned"]
          (g payload))))))
(show (run))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttLexicalCallable.mon"
            output = directory / "qtt-lexical-callable"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [str(resolve_monad_binary()), str(program), "-o", str(output),
                 "--emit-llvm", "--trace=qtt-proof"],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            run_trace = compiled.stdout[compiled.stdout.index(
                "definition run"):]
            self.assertIn("typed Core: valid", run_trace)
            self.assertNotIn("INTERNAL ERROR", run_trace)
            self.assertIn("ownership ANF: valid", run_trace)
            self.assertIn("verdict: VERIFIED", run_trace)
            self.assertIn("move", run_trace)
            ir = program.with_suffix(".ll").read_text()
            self.assertIn("define ptr @run", ir)
            self.assertIn("call ptr @rt_unbox_string", ir)
            executed = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.strip(), "owned")

    def test_tail_recursive_transfer_fixed_point_reaches_source_pipeline(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(module QttRecursiveTransfer)
(define cycle-a
  (lambda ([value : String] -> String)
    (cycle-b value)))
(define cycle-b
  (lambda ([value : String] -> String)
    (cycle-a value)))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttRecursiveTransfer.mon"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(directory / "recursive-transfer"),
                    "--trace=qtt-proof",
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            self.assertRegex(
                compiled.stdout,
                r"transfer fixed point: module=.*QttRecursiveTransfer\.mon "
                r"candidates=\d+ proven=2 rejected=\d+ iterations=\d+",
            )
            self.assertIn("definition cycle-a", compiled.stdout)
            self.assertIn("definition cycle-b", compiled.stdout)
            # The greatest-fixed-point transfer theorem is proven, but a
            # borrowed entry capability may not be forged into the ownership
            # required by its consuming recursive edge.
            self.assertIn("module verdict: PARTIAL", compiled.stdout)

    def test_portable_demand_certificate_rejects_corruption(self):
        source = r'''
#include "qtt/pipeline.h"

#include <assert.h>

int main(void) {
    QttShadowDemandEvidence variable = {
        .node_id = 0x1234,
        .rule = QTT_DEMAND_RULE_VAR,
        .syntactic = { .finite = 1 },
        .runtime = { .finite = 1 },
        .local_use = true,
    };
    assert(qtt_shadow_demand_validate(&variable) ==
           QTT_SHADOW_DEMAND_VALID);

    variable.runtime = qtt_quantity_finite(0);
    assert(qtt_shadow_demand_validate(&variable) ==
           QTT_SHADOW_DEMAND_INVALID_QUANTITY);
    variable.runtime = qtt_quantity_finite(1);

    variable.node_id = 0;
    assert(qtt_shadow_demand_validate(&variable) ==
           QTT_SHADOW_DEMAND_INVALID_IDENTITY);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_evidence.c"
            output = directory / "qtt_evidence"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-iquote", str(ROOT / "src"),
                    str(harness),
                    str(ROOT / "src" / "qtt" / "evidence.c"),
                    str(ROOT / "src" / "qtt" / "quantity.c"),
                    "-o", str(output),
                ],
                cwd=ROOT, check=True,
            )
            subprocess.run([str(output)], cwd=ROOT, check=True)

    def test_compiler_policy_defaults_to_analysis_and_certified_memory(self):
        source = r'''
#include "qtt/compiler.h"

#include <assert.h>
#include <stdlib.h>

int main(void) {
    unsetenv("MONAD_QTT_MEMORY");
    unsetenv("MONAD_QTT_ANALYSIS");
    assert(qtt_compiler_analysis_enabled());
    assert(qtt_compiler_memory_enabled());
    assert(!qtt_compiler_diagnostics_enabled());

    setenv("MONAD_QTT_MEMORY", "0", 1);
    assert(!qtt_compiler_memory_enabled());
    assert(qtt_compiler_analysis_enabled());

    setenv("MONAD_QTT_ANALYSIS", "0", 1);
    assert(!qtt_compiler_analysis_enabled());
    setenv("MONAD_QTT_SHADOW", "1", 1);
    assert(qtt_compiler_diagnostics_enabled());

    uint64_t first = qtt_compiler_module_id("Module.mon");
    uint64_t again = qtt_compiler_module_id("Module.mon");
    uint64_t other = qtt_compiler_module_id("Other.mon");
    assert(first != 0);
    assert(first == again);
    assert(first != other);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_compiler.c"
            output = directory / "qtt_compiler"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-D_GNU_SOURCE",
                    "-iquote", str(ROOT / "src"),
                    str(harness), str(ROOT / "src" / "qtt" / "compiler.c"),
                    "-o", str(output),
                ],
                cwd=ROOT, check=True,
            )
            subprocess.run([str(output)], cwd=ROOT, check=True)

    def test_ordinary_hm_pipeline_attaches_grades_independently_of_diagnostics(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define identity
  (lambda ([value : Int] -> Int)
    value))
(define increment
  (lambda ([value : Int] -> Int)
    (+ value 1)))
(define invoke-identity
  (lambda ([value : Int] -> Int)
    (identity value)))
(define choose
  (lambda ([value : Int] -> Int)
    (if True value value)))
(define mutate-branch
  (lambda ([value : Int] [choose : Bool] -> Int)
    (begin
      (if choose (set! value 2) (set! value 3))
      value)))
(show (identity 2))
(show (invoke-identity 3))
(show (mutate-branch 1 True))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttCompilerPipeline.mon"
            program.write_text(source)

            def compile_with(extra_env, suffix):
                env = os.environ.copy()
                env["HOME"] = str(directory)
                env["MONAD_QTT_SHADOW"] = "1"
                env.update(extra_env)
                return subprocess.run(
                    [
                        str(resolve_monad_binary()), str(program),
                        "-o", str(directory / suffix),
                    ],
                    cwd=ROOT, env=env, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                )

            enabled = compile_with({}, "enabled")
            self.assertEqual(enabled.returncode, 0, enabled.stdout)
            self.assertNotIn("\x1b[", enabled.stdout)
            self.assertRegex(
                enabled.stdout,
                r"\[qtt-shadow\] identity: .*hm-scheme=1"
                r".*source-grades=1",
            )
            self.assertIn(
                "[qtt] PARTIAL  QttCompilerPipeline.mon",
                enabled.stdout,
            )
            self.assertIn(
                "coverage 4/5 verified · 1 conservative · 0 errors",
                enabled.stdout,
            )
            self.assertRegex(
                enabled.stdout,
                r"\[qtt-shadow\] mutate-branch: verified "
                r"\(4 blocks, \d+ instructions",
            )
            self.assertRegex(
                enabled.stdout,
                r"\[qtt-shadow\] invoke-identity: verified "
                r".*applications=1",
            )
            invoke_trace = enabled.stdout[enabled.stdout.index(
                "definition invoke-identity"):]
            invoke_trace = invoke_trace[:invoke_trace.index(
                "├─ definition choose")]
            self.assertIn("ownership ANF: valid", invoke_trace)
            self.assertRegex(
                invoke_trace, r"op i\d+: call  %v\d+ ← identity#",
            )
            self.assertIn(
                "[qtt] Quantitative ownership evidence",
                enabled.stdout,
            )
            self.assertIn("├─ definition identity", enabled.stdout)
            self.assertIn(
                "│  ├─ source shape: parameters=1 body-forms=1",
                enabled.stdout,
            )
            self.assertIn(
                "│  ├─ typed Core: valid",
                enabled.stdout,
            )
            self.assertIn(
                "│  ├─ quantitative signature: graded=1 erased=0",
                enabled.stdout,
            )
            self.assertIn("│  ├─ typing contexts", enabled.stdout)
            self.assertIn(
                "│  │  ├─ Σ global signature: module=",
                enabled.stdout,
            )
            self.assertIn(
                "│  │  ├─ Ψ modal/meta context: layer=0",
                enabled.stdout,
            )
            self.assertIn(
                "│  │  └─ Γ quantitative context: binders=1",
                enabled.stdout,
            )
            self.assertIn("│  ├─ binder contracts", enabled.stdout)
            self.assertRegex(
                enabled.stdout,
                r"│  │  └─ value #[0-9]+",
            )
            self.assertIn(
                "│  │     ├─ quantity: ρ_allowed=1 ρ_observed=1",
                enabled.stdout,
            )
            self.assertIn(
                "│  │     │  └─ subusage proof: 1 ≤ 1  ✓",
                enabled.stdout,
            )
            self.assertIn(
                "│  │     ├─ demand derivation",
                enabled.stdout,
            )
            self.assertRegex(
                enabled.stdout,
                r"│  │     │  └─ Var core=#[0-9a-f]{16} "
                r".*ρˢ=1 ρʳ=1",
            )
            self.assertIn(
                "portable certificate: valid-after-Core-free ✓",
                enabled.stdout,
            )
            self.assertRegex(
                enabled.stdout,
                r"│  │     ├─ ownership: (erased|borrowed|consumed|shared)",
            )
            self.assertRegex(
                enabled.stdout,
                r"│  │     ├─ representation: "
                r"(immediate|inline|owned-heap|foreign|unknown)",
            )
            self.assertIn("│  │     └─ canonical type: #", enabled.stdout)
            self.assertIn("│  ├─ result contract", enabled.stdout)
            self.assertRegex(
                enabled.stdout,
                r"│  │  ├─ capability: "
                r"(immediate|owned|borrowed|shared|unknown)",
            )
            self.assertRegex(
                enabled.stdout,
                r"│  │  ├─ provenance: "
                r"(immediate|static|transferred|fresh|unknown)",
            )
            self.assertIn("│  │  └─ contract digest: #", enabled.stdout)
            self.assertRegex(
                enabled.stdout,
                r"│  ├─ ownership ANF: valid blocks=\d+ instructions=\d+",
            )
            self.assertIn("│  │  └─ block b0", enabled.stdout)
            self.assertRegex(
                enabled.stdout,
                r"│  │     ├─ op i0: [a-z-]+",
            )
            self.assertRegex(
                enabled.stdout,
                r"│  │     ├─ op i0: [a-z-]+.*(?:←|⇒|κ|%v)",
            )
            self.assertRegex(
                enabled.stdout,
                r"│  │     └─ terminator: (return|jump|branch)",
            )
            self.assertIn(
                "│  ├─ certificate check: valid",
                enabled.stdout,
            )
            self.assertIn(
                "│  └─ verdict: VERIFIED — safe for certified QTT consumers",
                enabled.stdout,
            )
            self.assertIn("├─ definition choose", enabled.stdout)
            self.assertRegex(
                enabled.stdout,
                r"(?s)definition choose.*demand derivation"
                r".*If core=#[0-9a-f]{16} .*ρˢ=2 ρʳ=1"
                r".*choice equation: ρʳ = ρc \+ \(ρt ⊔ ρe\)"
                r".*├─ condition: Zero core=#[0-9a-f]{16}"
                r".*├─ then: Var core=#[0-9a-f]{16}"
                r".*└─ else: Var core=#[0-9a-f]{16}",
            )
            choose_proof = re.search(
                r"(?s)definition choose.*?demand derivation"
                r".*?then: Var core=#([0-9a-f]{16})"
                r".*?else: Var core=#([0-9a-f]{16})",
                enabled.stdout,
            )
            self.assertIsNotNone(choose_proof, enabled.stdout)
            self.assertNotEqual(
                choose_proof.group(1), choose_proof.group(2),
                "equal branch terms still require path-distinct node IDs",
            )
            self.assertRegex(
                enabled.stdout,
                r"terminator: branch %v\d+",
            )
            self.assertRegex(
                enabled.stdout,
                r"├─ then ⇒ b\d+\(.*\)",
            )
            self.assertRegex(
                enabled.stdout,
                r"└─ else ⇒ b\d+\(.*\)",
            )
            self.assertIn(
                "├─ definition increment",
                enabled.stdout,
            )
            self.assertIn(
                "│  │  └─ lowering obligation: unsupported-core-form",
                enabled.stdout,
            )
            self.assertIn(
                "│  ├─ certificate check: not reached",
                enabled.stdout,
            )
            self.assertIn(
                "│  │  └─ resource/control-flow obligations: pending",
                enabled.stdout,
            )
            self.assertIn(
                "│  └─ verdict: UNSUPPORTED — conservative codegen retained",
                enabled.stdout,
            )
            self.assertIn(
                "└─ module verdict: PARTIAL",
                enabled.stdout,
            )
            executed = subprocess.run(
                [str(directory / "enabled")],
                cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(executed.stdout.splitlines(), ["2", "3", "2"])

            colored = compile_with(
                {"MONAD_QTT_COLOR": "always"}, "colored")
            self.assertEqual(colored.returncode, 0, colored.stdout)
            self.assertIn(
                "\x1b[1;32mVERIFIED\x1b[0m",
                colored.stdout,
            )
            self.assertIn(
                "\x1b[1;33mUNSUPPORTED\x1b[0m",
                colored.stdout,
            )
            self.assertIn(
                "\x1b[1;33mPARTIAL\x1b[0m",
                colored.stdout,
            )
            self.assertIn(
                "\x1b[2mresource/control-flow obligations\x1b[0m",
                colored.stdout,
            )

            disabled = compile_with(
                {"MONAD_QTT_ANALYSIS": "0"}, "disabled")
            self.assertEqual(disabled.returncode, 0, disabled.stdout)
            line = next(
                line for line in disabled.stdout.splitlines()
                if "[qtt-shadow] identity:" in line
            )
            self.assertIn("hm-scheme=0", line)
            self.assertNotIn("source-grades=", line)

    def test_owned_string_replacement_reaches_registered_compiler_pipeline(self):
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """
(define replace-string
  (lambda ([_ : Int] -> String)
    (with [text "old"]
      (begin
        (set! text "replacement")
        text))))
(show (replace-string 0))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttOwnedReplacement.mon"
            executable = directory / "owned-replacement"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            env["MONAD_QTT_SHADOW"] = "1"
            compiled = subprocess.run(
                [
                    str(resolve_monad_binary()), str(program),
                    "-o", str(executable),
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            self.assertRegex(
                compiled.stdout,
                r"\[qtt-shadow\] replace-string: verified ",
            )
            executed = subprocess.run(
                [str(executable)],
                cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)
            self.assertEqual(
                executed.stdout.splitlines(), ["replacement"])

if __name__ == "__main__":
    unittest.main()
