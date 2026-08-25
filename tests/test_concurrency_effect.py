"""Cycle 3 concurrency operations require exact scoped effect authority."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ConcurrencyEffectTests(unittest.TestCase):
    def test_trace_effects_are_covered_by_the_scoped_qtt_row(self):
        source = r'''
#include "concurrency/effect.h"

#include <assert.h>

static ConcurrencyTaskId task(uint64_t id) {
    return (ConcurrencyTaskId){.module_id = 9, .task_id = id};
}

int main(void) {
    ConcurrencyStep steps[] = {
        concurrency_spawn(task(1), task(2), NULL, 0),
        concurrency_cancel(task(1), task(2)),
        concurrency_checkpoint(task(2)),
        concurrency_cleanup_complete(task(2)),
        concurrency_join(task(1), task(2)),
    };
    ConcurrencyTrace trace = {
        .root = task(1),
        .steps = steps,
        .step_count = 5,
    };
    QttEffectArena *arena = qtt_effect_arena_new();
    assert(arena);
    QttEffectRow *row = qtt_effect_empty(arena);
    for (int operation = CONCURRENCY_EFFECT_JOIN;
         operation >= CONCURRENCY_EFFECT_SPAWN; operation--) {
        QttEffectAtom atom = concurrency_effect_atom(
            (ConcurrencyEffectOperation)operation, 77, 88);
        row = qtt_effect_extend_atom(arena, &atom, row);
        assert(row);
    }
    QttEffectSolver *solver = qtt_effect_solver_new(arena);
    assert(solver);
    assert(concurrency_verify_trace_effects(
               &trace, solver, row, 77, 88) ==
           CONCURRENCY_EFFECTS_VALID);
    assert(concurrency_verify_trace_effects(
               &trace, solver, row, 78, 88) ==
           CONCURRENCY_EFFECT_MISSING);

    QttEffectAtom spawn = concurrency_effect_atom(
        CONCURRENCY_EFFECT_SPAWN, 77, 88);
    QttEffectRow *spawn_only = qtt_effect_extend_atom(
        arena, &spawn, qtt_effect_empty(arena));
    assert(concurrency_verify_trace_effects(
               &trace, solver, spawn_only, 77, 88) ==
           CONCURRENCY_EFFECT_MISSING);

    ConcurrencyStep exceptional[] = {
        concurrency_spawn(task(1), task(2), NULL, 0),
        concurrency_fail(task(2)),
        concurrency_cleanup_fail(task(2)),
        concurrency_cleanup_complete(task(2)),
        concurrency_join(task(1), task(2)),
    };
    trace.steps = exceptional;
    assert(concurrency_verify_trace_effects(
               &trace, solver, row, 77, 88) ==
           CONCURRENCY_EFFECTS_VALID);

    trace.steps = steps;
    steps[3] = concurrency_join(task(1), task(2));
    assert(concurrency_verify_trace_effects(
               &trace, solver, row, 77, 88) ==
           CONCURRENCY_EFFECT_INVALID_TRACE);
    qtt_effect_solver_free(solver);
    qtt_effect_arena_free(arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "effect_test.c"
            binary_path = Path(directory) / "effect_test"
            source_path.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(source_path),
                    str(ROOT / "concurrency" / "kernel.c"),
                    str(ROOT / "concurrency" / "effect.c"),
                    str(ROOT / "effects" / "effect.c"),
                    str(ROOT / "qtt" / "quantity.c"),
                    "-o", str(binary_path),
                ],
                check=True,
            )
            environment = os.environ.copy()
            environment["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(binary_path)], env=environment, check=True)


if __name__ == "__main__":
    unittest.main()
