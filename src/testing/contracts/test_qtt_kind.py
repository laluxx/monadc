"""Executable laws for the kind algebra beneath qualified HM schemes."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class QttKindTests(unittest.TestCase):
    def test_occurs_checked_unification_and_canonical_transport(self):
        source = r'''
#include "qtt/kind.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    QttKindArena *arena = qtt_kind_arena_new();
    QttKindSolver *solver = qtt_kind_solver_new(arena);
    assert(arena && solver);

    QttKind *k = qtt_kind_fresh(arena);
    QttKind *type = qtt_kind_type(arena);
    QttKind *row = qtt_kind_effect_row(arena);
    QttKind *constructor = qtt_kind_arrow(arena, type, type);
    assert(k && type && row && constructor);
    assert(qtt_kind_unify(solver, k, constructor) == QTT_KIND_SOLVED);
    assert(qtt_kind_equal(solver, k, constructor));

    QttKind *cycle = qtt_kind_fresh(arena);
    QttKind *recursive = qtt_kind_arrow(arena, type, cycle);
    assert(qtt_kind_unify(solver, cycle, recursive) == QTT_KIND_OCCURS);

    /* Failed structural unification is transactional: speculative bindings
       must not poison a later constraint batch. */
    QttKind *transactional = qtt_kind_fresh(arena);
    QttKind *left = qtt_kind_arrow(arena, transactional, transactional);
    QttKind *right = qtt_kind_arrow(arena, type, row);
    assert(qtt_kind_unify(solver, left, right) == QTT_KIND_MISMATCH);
    assert(qtt_kind_unify(solver, transactional, row) == QTT_KIND_SOLVED);

    char *encoded = qtt_kind_serialize(solver, constructor);
    assert(encoded && strcmp(encoded, "monad-kind-v1|(*->*)") == 0);
    QttKindArena *decoded_arena = qtt_kind_arena_new();
    QttKind *decoded = qtt_kind_deserialize(decoded_arena, encoded);
    assert(decoded);
    char *roundtrip = qtt_kind_serialize(NULL, decoded);
    assert(roundtrip && strcmp(encoded, roundtrip) == 0);
    assert(qtt_kind_fingerprint(solver, constructor) ==
           qtt_kind_fingerprint(NULL, decoded));
    assert(!qtt_kind_deserialize(decoded_arena, "monad-kind-v1|(*->)"));
    assert(strcmp(qtt_kind_name(row), "EffectRow") == 0);

    free(roundtrip);
    free(encoded);
    qtt_kind_arena_free(decoded_arena);
    qtt_kind_solver_free(solver);
    qtt_kind_arena_free(arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "kind_test.c"
            executable = directory / "kind_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT / "src"), str(harness),
                    str(ROOT / "src" / "qtt" / "kind.c"), "-o", str(executable),
                ], cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
