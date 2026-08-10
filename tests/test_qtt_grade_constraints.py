"""Symbolic grades support constraints and fresh polymorphic instantiation."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttGradeConstraintTests(unittest.TestCase):
    def test_solver_and_grade_scheme_freshness(self):
        source = r'''
#include "qtt/constraints.h"
#include <assert.h>
#include <stdlib.h>

int main(void) {
    QttGradeArena *arena = qtt_grade_arena_new();
    QttGradeSolver *solver = qtt_grade_solver_new(arena);
    assert(arena && solver);
    QttGradeExpr *alpha = qtt_grade_fresh(arena);
    QttGradeExpr *one =
        qtt_grade_constant(arena, qtt_quantity_finite(1));
    QttGradeExpr *two =
        qtt_grade_constant(arena, qtt_quantity_finite(2));
    assert(alpha && one && two);
    assert(qtt_grade_constrain_leq(solver, one, alpha));
    assert(qtt_grade_constrain_leq(solver, alpha, two));
    assert(qtt_grade_solve(solver) == QTT_GRADE_SOLVED);
    QttQuantity solution = qtt_grade_solution(solver, alpha);
    assert(!solution.is_omega && solution.finite == 1);

    QttGradeSolver *inconsistent = qtt_grade_solver_new(arena);
    assert(qtt_grade_constrain_leq(inconsistent, two, one));
    assert(qtt_grade_solve(inconsistent) == QTT_GRADE_UNSATISFIABLE);

    QttGradeExpr *beta = qtt_grade_fresh(arena);
    QttGradeExpr *beta_plus_one = qtt_grade_add(arena, beta, one);
    QttGradeSolver *cyclic = qtt_grade_solver_new(arena);
    assert(qtt_grade_constrain_equal(cyclic, beta, beta_plus_one));
    assert(qtt_grade_solve(cyclic) == QTT_GRADE_OCCURS_CYCLE);

    QttGradeExpr *domains[] = {alpha, one};
    QttGradeScheme *scheme =
        qtt_grade_generalize(arena, domains, 2);
    assert(scheme && qtt_grade_scheme_quantified_count(scheme) == 1);
    QttGradeExpr **first = qtt_grade_instantiate(arena, scheme);
    QttGradeExpr **second = qtt_grade_instantiate(arena, scheme);
    assert(first && second);
    assert(qtt_grade_expr_kind(first[0]) == QTT_GRADE_VARIABLE);
    assert(qtt_grade_expr_kind(second[0]) == QTT_GRADE_VARIABLE);
    assert(qtt_grade_variable_id(first[0]) !=
           qtt_grade_variable_id(second[0]));
    assert(qtt_grade_expr_kind(first[1]) == QTT_GRADE_CONSTANT);
    assert(qtt_grade_constant_value(first[1]).finite == 1);

    QttGradeExpr *three =
        qtt_grade_constant(arena, qtt_quantity_finite(3));
    QttGradeExpr *alpha_twice = qtt_grade_add(arena, alpha, alpha);
    QttGradeExpr *specialized = qtt_grade_substitute(
        arena, alpha_twice, alpha, three);
    assert(specialized);
    QttGradeSolver *specialized_solver = qtt_grade_solver_new(arena);
    assert(specialized_solver);
    assert(qtt_grade_solve(specialized_solver) == QTT_GRADE_SOLVED);
    assert(qtt_grade_solution(
               specialized_solver, specialized).finite == 6);
    qtt_grade_solver_free(specialized_solver);

    QttGradeExpr *rigid = qtt_grade_fresh(arena);
    QttGradeExpr *mixed[] = {alpha, rigid};
    uint64_t rigid_ids[] = {qtt_grade_variable_id(rigid)};
    QttGradeScheme *scoped = qtt_grade_generalize_excluding(
        arena, mixed, 2, rigid_ids, 1);
    assert(scoped && qtt_grade_scheme_quantified_count(scoped) == 1);
    QttGradeExpr **scoped_instance =
        qtt_grade_instantiate(arena, scoped);
    assert(scoped_instance);
    assert(qtt_grade_variable_id(scoped_instance[0]) !=
           qtt_grade_variable_id(alpha));
    assert(qtt_grade_variable_id(scoped_instance[1]) ==
           qtt_grade_variable_id(rigid));

    free(scoped_instance);
    qtt_grade_scheme_free(scoped);
    free(first);
    free(second);
    qtt_grade_scheme_free(scheme);
    qtt_grade_solver_free(cyclic);
    qtt_grade_solver_free(inconsistent);
    qtt_grade_solver_free(solver);
    qtt_grade_arena_free(arena);

    QttGradeArena *source_arena = qtt_grade_arena_new();
    QttGradeExpr *source_variable = qtt_grade_fresh(source_arena);
    QttGradeExpr *source_terms[] = {source_variable};
    QttGradeScheme *long_lived =
        qtt_grade_generalize(source_arena, source_terms, 1);
    assert(long_lived);
    qtt_grade_arena_free(source_arena);
    QttGradeArena *target_arena = qtt_grade_arena_new();
    QttGradeExpr **after_scope =
        qtt_grade_instantiate(target_arena, long_lived);
    assert(after_scope && qtt_grade_variable_id(after_scope[0]));
    QttGradeScheme *shared = qtt_grade_scheme_retain(long_lived);
    qtt_grade_scheme_free(long_lived);
    QttGradeExpr **after_owner_free =
        qtt_grade_instantiate(target_arena, shared);
    assert(after_owner_free);
    free(after_owner_free);
    free(after_scope);
    qtt_grade_scheme_free(shared);
    qtt_grade_arena_free(target_arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_grade_constraints_test.c"
            executable = directory / "qtt_grade_constraints_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "quantity.c"),
                    str(ROOT / "qtt" / "constraints.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
