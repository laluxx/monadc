"""Reusable typed-Core to symbolic-usage producer."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class QttCoreUsageTests(unittest.TestCase):
    def test_producer_is_independent_from_coherence(self):
        source = r'''
#include "qtt/core_usage.h"
#include <assert.h>
int main(void) {
    QttCoreVar x = {.module_id = 2, .binder_id = 3};
    QttCoreNode use = {.kind = QTT_CORE_VAR, .var = x};
    QttGradeArena *arena = qtt_grade_arena_new();
    QttCoreUsageResult result = qtt_core_usage_lower(arena, &use);
    assert(result.status == QTT_CORE_USAGE_OK);
    assert(qtt_usage_grade(result.usage, x.binder_id));
    qtt_usage_context_free(result.usage);
    QttCoreNode replacement = {
        .kind = QTT_CORE_GLOBAL, .global = {.name = "replacement"}};
    QttCoreNode write = {
        .kind = QTT_CORE_WRITE,
        .write = {
            .place = {.root = x},
            .value = &replacement,
        },
    };
    result = qtt_core_usage_lower(arena, &write);
    assert(result.status == QTT_CORE_USAGE_OK);
    QttGradeExpr *write_grade =
        qtt_usage_grade(result.usage, x.binder_id);
    QttGradeSolver *write_solver = qtt_grade_solver_new(arena);
    assert(write_grade && write_solver);
    assert(qtt_grade_solve(write_solver) == QTT_GRADE_SOLVED);
    assert(qtt_grade_solution(
               write_solver, write_grade).finite == 1);
    qtt_grade_solver_free(write_solver);
    qtt_usage_context_free(result.usage);
    QttCoreNode unknown = {
        .kind = QTT_CORE_GLOBAL, .global = {.name = "f"}};
    QttCoreNode call = {
        .kind = QTT_CORE_APPLY,
        .apply = {.callee = &unknown, .argument_count = 0}};
    result = qtt_core_usage_lower(arena, &call);
    assert(result.status == QTT_CORE_USAGE_UNSUPPORTED);
    assert(result.boundary == QTT_CORE_USAGE_BOUNDARY_APPLICATION);
    assert(result.offending == &call);
    qtt_grade_arena_free(arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "core_usage.c"
            executable = directory / "core_usage"
            harness.write_text(source)
            subprocess.run([
                "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                "-iquote", str(ROOT / "src"), str(harness),
                str(ROOT / "src" / "qtt" / "core.c"),
                str(ROOT / "src" / "qtt" / "quantity.c"),
                str(ROOT / "src" / "qtt" / "demand.c"),
                str(ROOT / "src" / "qtt" / "graded.c"),
                str(ROOT / "src" / "qtt" / "resource.c"),
                str(ROOT / "src" / "qtt" / "type_identity.c"),
                str(ROOT / "src" / "qtt" / "signature.c"),
                str(ROOT / "src" / "effects" / "effect.c"),
                str(ROOT / "src" / "qtt" / "place.c"),
                str(ROOT / "src" / "qtt" / "signature_env.c"),
                str(ROOT / "src" / "qtt" / "environment.c"),
                str(ROOT / "src" / "qtt" / "constraints.c"),
                str(ROOT / "src" / "qtt" / "elaboration.c"),
                str(ROOT / "src" / "qtt" / "core_usage.c"),
                "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
