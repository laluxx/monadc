"""QTT shadow verification runs on real post-inference compiler input."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttPipelineTests(unittest.TestCase):
    def test_real_compiler_reports_verified_and_unsupported_definitions(self):
        compiler = ROOT / "monad"
        if not compiler.exists():
            subprocess.run(["make", "-j2"], cwd=ROOT, check=True)
        source = """\
(module Main)
(define answer (lambda () 42))
(define identity (lambda ([x : Int] -> Int) x))
(define qtt-unused (lambda ([x : Int] -> Int) 42))
(define qtt-twice (lambda ([x : Int] -> Int) (+ x x)))
(define qtt-call-twice (lambda ([x : Int] -> Int) (qtt-twice x)))
(define qtt-branch (lambda ([x : Int] -> Int) (if True x x)))
(define qtt-let-unused (lambda ([x : Int] -> Int) (with [y x] 42)))
(define qtt-let-twice (lambda ([x : Int] -> Int) (with [y x] (+ y y))))
(define qtt-let-chain (lambda ([x : Int] -> Int)
  (with [y x z y] (+ z z))))
(define qtt-let-shadow (lambda ([x : Int] -> Int)
  (with [x (+ x 1)] x)))
(define qtt-let-effect (lambda ([x : Int] -> Int)
  (with [y (show x)] 42)))
(define qtt-let-partial (lambda ([x : Int] -> Int)
  (with [y (/ 1 x)] 42)))
(define qtt-capture
  (lambda ([x : Int] -> Fn)
    (lambda ([y : Int] -> Int) (+ x x y))))
(define qtt-capture-unused
  (lambda ([x : Int] -> Fn)
    (lambda ([y : Int] -> Int) y)))
(define qtt-two-captures
  (lambda ([x : Int] -> Fn)
    (begin
      (lambda ([y : Int] -> Int) (+ x y))
      (lambda ([z : Int] -> Int) (+ x x z)))))
(define qtt-forward-capture
  (lambda ([x : Int] -> Fn)
    (qtt-capture x)))
(define qtt-local-capture
  (lambda ([x : Int] -> Fn)
    (with [local x]
      (lambda ([y : Int] -> Int) (+ local y)))))
(define qtt-invoke-capture
  (lambda ([x : Int] -> Int)
    ((qtt-capture x) 1)))
(define qtt-invoke-capture-unused
  (lambda ([x : Int] -> Int)
    ((qtt-capture-unused x) 1)))
(define qtt-invoke-local
  (lambda ([x : Int] -> Int)
    ((qtt-local-capture x) 1)))
(define qtt-branch-capture
  (lambda ([x : Int] [choose : Bool] -> Fn)
    (if choose
        (lambda ([y : Int] -> Int) (+ x y))
        (lambda ([y : Int] -> Int) (+ x x y)))))
(define qtt-invoke-branch-capture
  (lambda ([x : Int] -> Int)
    ((qtt-branch-capture x True) 1)))
(define qtt-domain-twice
  (lambda ([captured : Int] -> Fn)
    (lambda ([value : Int] -> Int)
      (+ captured value value))))
(define qtt-invoke-domain-twice
  (lambda ([x : Int] -> Int)
    ((qtt-domain-twice 1) x)))
(define qtt-invoke-two-sites
  (lambda ([x : Int] -> Int)
    (begin
      ((qtt-capture x) 1)
      ((qtt-capture x) 2))))
(define qtt-invoke-forward-capture
  (lambda ([x : Int] -> Int)
    ((qtt-forward-capture x) 1)))
(define qtt-forward-complex-capture
  (lambda ([x : Int] -> Fn)
    (qtt-capture (+ x 1))))
(define qtt-forward-local-factory
  (lambda ([x : Int] -> Fn)
    (with [local x]
      (qtt-capture local))))
(define qtt-invoke-complex-capture
  (lambda ([x : Int] -> Int)
    ((qtt-capture (+ x 1)) 1)))
(define qtt-invoke-forward-complex
  (lambda ([x : Int] -> Int)
    ((qtt-forward-complex-capture x) 1)))
(define qtt-invoke-forward-local
  (lambda ([x : Int] -> Int)
    ((qtt-forward-local-factory x) 1)))
(define qtt-invoke-bound
  (lambda ([x : Int] -> Int)
    (with [f (qtt-capture x)]
      (f 1))))
(define qtt-invoke-bound-complex
  (lambda ([x : Int] -> Int)
    (with [f (qtt-capture (+ x 1))]
      (f 1))))
(define qtt-invoke-bound-unused
  (lambda ([x : Int] -> Int)
    (with [f (qtt-capture x)]
      42)))
(define qtt-invoke-bound-alias
  (lambda ([x : Int] -> Int)
    (with [f (qtt-capture x)
           g f]
      (g 1))))
(define qtt-apply-constant
  (lambda ([f : Fn] -> Int)
    (f 1)))
(define qtt-invoke-parameter
  (lambda ([x : Int] -> Int)
    (qtt-apply-constant (qtt-capture x))))
(define qtt-apply-twice
  (lambda ([f : Fn] -> Int)
    (begin
      (f 1)
      (f 2))))
(define qtt-invoke-parameter-twice
  (lambda ([x : Int] -> Int)
    (qtt-apply-twice (qtt-capture x))))
(define qtt-apply-branch
  (lambda ([f : Fn] [choose : Bool] -> Int)
    (if choose
        (f 1)
        (f 2))))
(define qtt-invoke-parameter-branch
  (lambda ([x : Int] -> Int)
    (qtt-apply-branch (qtt-capture x) True)))
(define qtt-apply-open
  (lambda ([f : Fn] [x : Int] -> Int)
    (f x)))
(define qtt-invoke-parameter-open
  (lambda ([x : Int] -> Int)
    (qtt-apply-open (qtt-domain-twice 1) x)))
(define qtt-two-domain-factory
  (lambda ([seed : Int] -> Fn)
    (lambda ([left : Int] [right : Int] -> Int)
      (+ left right right))))
(define qtt-apply-two-open
  (lambda ([f : Fn] [x : Int] [y : Int] -> Int)
    (f x y)))
(define qtt-invoke-two-open
  (lambda ([x : Int] [y : Int] -> Int)
    (qtt-apply-two-open (qtt-two-domain-factory 0) x y)))
(define qtt-branch-domain-factory
  (lambda ([choose : Bool] -> Fn)
    (if choose
        (lambda ([value : Int] -> Int) value)
        (lambda ([value : Int] -> Int) (+ value value)))))
(define qtt-invoke-branch-domain
  (lambda ([x : Int] -> Int)
    (qtt-apply-open (qtt-branch-domain-factory True) x)))
(define use-identity (lambda () (identity 7)))
(define transfer (lambda ([x : Int] -> Int)
  ((lambda ([y : Int] -> Int) y) x)))
(define qtt-mutate (lambda ([x : Int] -> Int)
  (begin (set! x 2) x)))
(show (answer))
(show (qtt-mutate 1))
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program = directory / "QttShadow.mon"
            output = directory / "qtt-shadow"
            program.write_text(source)
            env = os.environ.copy()
            env["MONAD_QTT_SHADOW"] = "1"
            env["HOME"] = str(directory)
            result = subprocess.run(
                [str(compiler), str(program), "-o", str(output)],
                cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("[qtt-shadow] answer: verified", result.stdout)
            self.assertIn("[qtt-shadow] identity: verified", result.stdout)
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-unused: .*source-grades=0")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-twice: .*source-grades=2")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-call-twice: .*source-grades=2")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-branch: .*source-grades=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-let-unused: .*source-grades=0")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-let-twice: .*source-grades=2")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-let-chain: .*source-grades=2")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-let-shadow: .*source-grades=1")
            effect_line = next(
                line for line in result.stdout.splitlines()
                if "[qtt-shadow] qtt-let-effect:" in line)
            partial_line = next(
                line for line in result.stdout.splitlines()
                if "[qtt-shadow] qtt-let-partial:" in line)
            self.assertNotIn("source-grades=", effect_line)
            self.assertNotIn("source-grades=", partial_line)
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-capture: .*source-grades=1"
                r".*closure-entries=1.*closure-param-entries=1"
                r".*closure-domains=1"
                r".*closure-latent=2"
                r".*result-closures=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-capture-unused: .*source-grades=0")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-two-captures: .*source-grades=2"
                r".*closure-entries=2.*closure-param-entries=2"
                r".*closure-domains=2"
                r".*closure-latent=1,2"
                r".*result-closures=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-forward-capture: .*source-grades=1"
                r".*closure-entries=1.*closure-param-entries=1"
                r".*closure-domains=1.*result-closures=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-local-capture: .*source-grades=1"
                r".*closure-entries=1.*closure-param-entries=0"
                r".*closure-latent=1.*result-closures=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-capture: .*source-grades=3"
                r".*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-capture-unused: "
                r".*source-grades=0")
            local_invocation = next(
                line for line in result.stdout.splitlines()
                if "[qtt-shadow] qtt-invoke-local:" in line)
            self.assertRegex(
                local_invocation,
                r"source-grades=1.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-branch-capture: "
                r".*source-grades=1,1.*closure-entries=2"
                r".*result-closures=2")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-branch-capture: "
                r".*source-grades=3.*invoked-closure-instances=2")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-domain-twice: "
                r".*source-grades=2")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-two-sites: "
                r".*source-grades=6.*invoked-closure-instances=2")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-forward-capture: "
                r".*source-grades=3.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-forward-complex-capture: "
                r".*source-grades=1.*result-closures=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-forward-local-factory: "
                r".*source-grades=1.*result-closures=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-complex-capture: "
                r".*source-grades=1.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-forward-complex: "
                r".*source-grades=1.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-forward-local: "
                r".*source-grades=1.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-bound: "
                r".*source-grades=3.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-bound-complex: "
                r".*source-grades=1.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-bound-unused: "
                r".*source-grades=0.*invoked-closure-instances=0")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-bound-alias: "
                r".*source-grades=3.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-apply-constant: "
                r".*source-grades=1.*callable-parameters=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-parameter: "
                r".*source-grades=3.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-apply-twice: "
                r".*source-grades=2.*callable-parameters=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-parameter-twice: "
                r".*source-grades=5.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-apply-branch: "
                r".*source-grades=1,1.*callable-parameters=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-parameter-branch: "
                r".*source-grades=3.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-apply-open: "
                r".*source-grades=1,0.*callable-parameters=1"
                r".*callable-domains=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-parameter-open: "
                r".*source-grades=2.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-apply-two-open: "
                r".*source-grades=1,0,0.*callable-parameters=1"
                r".*callable-domains=2")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-two-open: "
                r".*source-grades=1,2.*invoked-closure-instances=1")
            self.assertRegex(
                result.stdout,
                r"\[qtt-shadow\] qtt-invoke-branch-domain: "
                r".*source-grades=2.*invoked-closure-instances=2")
            self.assertIn("[qtt-shadow] use-identity: verified", result.stdout)
            self.assertIn("[qtt-shadow] transfer: verified", result.stdout)
            self.assertIn("[qtt-shadow] qtt-mutate: verified", result.stdout)
            execution = subprocess.run(
                [str(output)], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(execution.returncode, 0, execution.stdout)
            self.assertEqual(execution.stdout.splitlines()[-1], "2")
            self.assertIn("grades=1", result.stdout)
            self.assertIn("hm-scheme=1", result.stdout)
            self.assertIn("applications=1", result.stdout)
            run = subprocess.run(
                [str(output)], text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("42", run.stdout)


if __name__ == "__main__":
    unittest.main()
