"""End-to-end regression tests for compiler-visible QTT analysis."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MONAD = ROOT / "monad"
FIXTURE = ROOT / "tests" / "qtt_usage_report.mon"


class QttReportTests(unittest.TestCase):
    def test_projected_mutation_reports_compiler_generated_loan(self):
        fixture = ROOT / "tests" / "qtt_projected_mutation.mon"
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            output = directory / "projected-mutation"
            env = os.environ.copy()
            env["HOME"] = str(directory)
            env["MONAD_CORE"] = str(ROOT / "core")
            result = subprocess.run(
                [str(MONAD), str(fixture), "--trace=qtt-proof",
                 "-o", str(output)],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            execution = subprocess.run(
                [str(output)], cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            ) if result.returncode == 0 else None
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("├─ definition mutate-x", result.stdout)
        self.assertIn("borrow-exclusive", result.stdout)
        self.assertIn("parent=root", result.stdout)
        self.assertIn(
            "backend authority: certified ownership cleanup", result.stdout)
        self.assertIn(
            "reclaim value#", result.stdout)
        self.assertNotIn("cleanup fallback:", result.stdout)
        self.assertIsNotNone(execution)
        self.assertEqual(execution.returncode, 0, execution.stdout)
        self.assertEqual(execution.stdout.strip(), "99")

    def test_trace_qtt_is_a_concise_user_module_report(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "qtt-report"
            env = os.environ.copy()
            env["HOME"] = directory
            env["MONAD_CORE"] = str(ROOT / "core")
            result = subprocess.run(
                [str(MONAD), str(FIXTURE), "--trace=qtt", "-o", str(output)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("[qtt] PARTIAL  qtt_usage_report.mon", result.stdout)
        self.assertIn(
            "coverage 0/2 verified · 2 conservative · 0 errors",
            result.stdout,
        )
        self.assertIn("inspect  --trace=qtt-proof", result.stdout)
        self.assertNotIn("running quantitative ownership", result.stdout)
        self.assertNotIn("module summary:", result.stdout)
        self.assertNotIn("├─ definition", result.stdout)
        self.assertNotIn("[qtt] choose/x#", result.stdout)
        self.assertNotIn("[qtt] import contract", result.stdout)

    def test_proof_colors_semantic_labels_and_success_marks(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "qtt-color"
            env = os.environ.copy()
            env["HOME"] = directory
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_QTT_COLOR"] = "always"
            result = subprocess.run(
                [str(MONAD), str(FIXTURE), "--trace=qtt-proof", "-o", str(output)],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("\x1b[36mquantity:\x1b[0m", result.stdout)
        self.assertIn("\x1b[36mownership:\x1b[0m", result.stdout)
        self.assertIn("\x1b[36mrepresentation:\x1b[0m", result.stdout)
        self.assertIn("\x1b[32m✓\x1b[0m", result.stdout)

    def test_trace_qtt_proof_exposes_the_user_proof_without_dependency_noise(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "qtt-proof"
            env = os.environ.copy()
            env["HOME"] = directory
            env["MONAD_CORE"] = str(ROOT / "core")
            result = subprocess.run(
                [str(MONAD), str(FIXTURE), "--trace=qtt-proof", "-o", str(output)],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("├─ definition choose", result.stdout)
        self.assertIn("Σ ; Ψ | Γ ⊢ℓ choose : A ! ε ▷ Δ", result.stdout)
        self.assertIn("[qtt] choose/x#1 = choice(1,2)", result.stdout)
        self.assertNotIn("[qtt] import contract", result.stdout)

    def test_verbose_levels_expose_real_compiler_qtt_pipeline(self):
        source = """
(define fresh-prefix
  (lambda ([text : String] -> String)
    (__rt_string_take text 3)))
(define reclaim
  (lambda ()
    (with [text (fresh-prefix "abcdef")]
      7)))
(show (reclaim))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "VerboseQtt.mon"
            program.write_text(source)
            env = os.environ.copy()
            env["HOME"] = str(directory)
            env["MONAD_CORE"] = str(ROOT / "core")

            concise = subprocess.run(
                [
                    str(MONAD), str(program), "-v",
                    "-o", str(directory / "concise"),
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(concise.returncode, 0, concise.stdout)
            self.assertIn(
                "[qtt] running quantitative ownership analysis",
                concise.stdout,
            )
            self.assertIn("[qtt] PARTIAL  VerboseQtt.mon", concise.stdout)
            self.assertIn("coverage 0/2 verified", concise.stdout)
            self.assertNotIn("[qtt] module summary:", concise.stdout)
            self.assertNotIn("[qtt] fresh-prefix/text#", concise.stdout)
            self.assertNotIn("[qtt] reclaim text#", concise.stdout)

            detailed = subprocess.run(
                [
                    str(MONAD), str(program), "-vv",
                    "-o", str(directory / "detailed"),
                ],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(detailed.returncode, 0, detailed.stdout)
            self.assertIn("[qtt] fresh-prefix/text#", detailed.stdout)
            self.assertIn(
                "[qtt] contract fresh-prefix: result=owned/fresh",
                detailed.stdout,
            )
            self.assertIn(
                "[qtt] reclaim text#",
                detailed.stdout,
            )
            self.assertIn("materialization=owned-result", detailed.stdout)
            self.assertIn("destructor=free", detailed.stdout)
            self.assertIn(
                "[qtt] backend authority: certified ownership cleanup",
                detailed.stdout,
            )


if __name__ == "__main__":
    unittest.main()
