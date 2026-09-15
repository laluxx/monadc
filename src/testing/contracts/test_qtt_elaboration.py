"""Compositional symbolic usage equations for quantitative elaboration."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class QttElaborationTests(unittest.TestCase):
    def test_sequence_choice_scaling_and_allowance_constraints(self):
        source = r'''
#include "qtt/elaboration.h"
#include <assert.h>
#include <stdlib.h>

int main(void) {
    QttGradeArena *arena = qtt_grade_arena_new();
    QttGradeExpr *one = qtt_grade_constant(
        arena, qtt_quantity_finite(1));
    QttGradeExpr *two = qtt_grade_constant(
        arena, qtt_quantity_finite(2));
    QttUsageContext *x = qtt_usage_singleton(arena, 41);
    assert(arena && one && two && x);

    QttUsageContext *callee_uses_twice =
        qtt_usage_scale(arena, two, x);
    QttUsageContext *two_sequential_uses =
        qtt_usage_sequence(arena, x, x);
    QttUsageContext *exclusive_uses =
        qtt_usage_choice(arena, x, x);
    assert(callee_uses_twice && two_sequential_uses && exclusive_uses);

    QttGradeSolver *solver = qtt_grade_solver_new(arena);
    assert(qtt_usage_constrain_binder(
        solver, callee_uses_twice, 41, two));
    assert(qtt_usage_constrain_binder(
        solver, two_sequential_uses, 41, two));
    assert(qtt_usage_constrain_binder(
        solver, exclusive_uses, 41, one));
    assert(qtt_grade_solve(solver) == QTT_GRADE_SOLVED);
    assert(qtt_grade_solution(
        solver, qtt_usage_grade(exclusive_uses, 41)).finite == 1);
    assert(qtt_grade_solution(
        solver, qtt_usage_grade(two_sequential_uses, 41)).finite == 2);

    QttGradeSolver *reject = qtt_grade_solver_new(arena);
    assert(qtt_usage_constrain_binder(
        reject, two_sequential_uses, 41, one));
    assert(qtt_grade_solve(reject) == QTT_GRADE_UNSATISFIABLE);

    QttGradeExpr *rho = qtt_grade_fresh(arena);
    QttUsageContext *symbolic = qtt_usage_scale(arena, rho, x);
    QttGradeSolver *infer = qtt_grade_solver_new(arena);
    assert(qtt_usage_constrain_binder(infer, symbolic, 41, two));
    assert(qtt_grade_constrain_leq(infer, two, rho));
    assert(qtt_grade_solve(infer) == QTT_GRADE_SOLVED);
    assert(qtt_grade_solution(infer, rho).finite == 2);
    QttGradeExpr *symbolic_choice =
        qtt_grade_maximum(arena, rho, one);
    QttGradeExpr *templates[] = {symbolic_choice};
    QttGradeScheme *choice_scheme =
        qtt_grade_generalize(arena, templates, 1);
    QttGradeExpr **choice_instance =
        qtt_grade_instantiate(arena, choice_scheme);
    assert(choice_scheme && choice_instance);
    assert(qtt_grade_expr_kind(choice_instance[0]) ==
           QTT_GRADE_MAXIMUM);

    /* Context operations are BinderId-based and preserve disjoint binders. */
    QttUsageContext *y = qtt_usage_singleton(arena, 99);
    QttUsageContext *xy = qtt_usage_sequence(arena, x, y);
    assert(qtt_usage_binding_count(xy) == 2);
    assert(qtt_grade_solution(
        solver, qtt_usage_grade(xy, 41)).finite == 1);
    assert(qtt_grade_solution(
        solver, qtt_usage_grade(xy, 99)).finite == 1);

    QttUsageContext *callee = qtt_usage_singleton(arena, 7);
    const QttUsageContext *arguments[] = {x, x};
    QttGradeExpr *domain_grades[] = {two, one};
    QttUsageContext *application = qtt_usage_application(
        arena, callee, arguments, domain_grades, 2);
    assert(application && qtt_usage_binding_count(application) == 2);
    assert(qtt_grade_solution(
        solver, qtt_usage_grade(application, 7)).finite == 1);
    assert(qtt_grade_solution(
        solver, qtt_usage_grade(application, 41)).finite == 3);
    assert(!qtt_usage_application(
        arena, callee, NULL, domain_grades, 2));

    QttUsageContext *local = qtt_usage_singleton(arena, 500);
    QttUsageContext *local_twice =
        qtt_usage_sequence(arena, local, local);
    QttUsageContext *let_twice =
        qtt_usage_let(arena, 500, x, local_twice);
    assert(let_twice);
    assert(!qtt_usage_grade(let_twice, 500));
    assert(qtt_grade_solution(
        solver, qtt_usage_grade(let_twice, 41)).finite == 2);
    QttUsageContext *empty = qtt_usage_empty(arena);
    QttUsageContext *let_unused =
        qtt_usage_let(arena, 500, x, empty);
    assert(let_unused && qtt_usage_binding_count(let_unused) == 0);

    QttUsageContext *parameter = qtt_usage_singleton(arena, 600);
    QttUsageContext *closure_body_left =
        qtt_usage_sequence(arena, x, x);
    QttUsageContext *closure_body = qtt_usage_sequence(
        arena, closure_body_left, parameter);
    uint64_t closure_parameters[] = {600};
    QttUsageClosure *closure = qtt_usage_closure(
        arena, closure_parameters, 1, closure_body);
    assert(closure);
    assert(!qtt_usage_grade(
        qtt_usage_closure_latent(closure), 600));
    assert(qtt_grade_solution(
        solver,
        qtt_usage_grade(qtt_usage_closure_captures(closure), 41)).finite == 1);
    assert(qtt_grade_solution(
        solver,
        qtt_usage_grade(qtt_usage_closure_latent(closure), 41)).finite == 2);
    QttUsageContext *invoked_three = qtt_usage_closure_invoke(
        arena, closure,
        qtt_grade_constant(arena, qtt_quantity_finite(3)));
    assert(invoked_three);
    assert(qtt_grade_solution(
        solver, qtt_usage_grade(invoked_three, 41)).finite == 6);

    qtt_usage_context_free(invoked_three);
    qtt_usage_closure_free(closure);
    qtt_usage_context_free(closure_body);
    qtt_usage_context_free(closure_body_left);
    qtt_usage_context_free(parameter);
    qtt_usage_context_free(let_unused);
    qtt_usage_context_free(empty);
    qtt_usage_context_free(let_twice);
    qtt_usage_context_free(local_twice);
    qtt_usage_context_free(local);
    qtt_usage_context_free(application);
    qtt_usage_context_free(callee);
    free(choice_instance);
    qtt_grade_scheme_free(choice_scheme);
    qtt_usage_context_free(xy);
    qtt_usage_context_free(y);
    qtt_usage_context_free(symbolic);
    qtt_grade_solver_free(infer);
    qtt_grade_solver_free(reject);
    qtt_grade_solver_free(solver);
    qtt_usage_context_free(exclusive_uses);
    qtt_usage_context_free(two_sequential_uses);
    qtt_usage_context_free(callee_uses_twice);
    qtt_usage_context_free(x);
    qtt_grade_arena_free(arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_elaboration_test.c"
            executable = directory / "qtt_elaboration_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT / "src"), str(harness),
                    str(ROOT / "src" / "qtt" / "quantity.c"),
                    str(ROOT / "src" / "qtt" / "constraints.c"),
                    str(ROOT / "src" / "qtt" / "elaboration.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
