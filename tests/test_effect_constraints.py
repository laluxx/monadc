"""Effect obligations distinguish proofs, refutations, and open residuals."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EffectConstraintTests(unittest.TestCase):
    def test_constraint_solver_and_certificate_replay(self):
        source = r'''
#include "effects/constraints.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    QttEffectArena *arena = qtt_effect_arena_new();
    QttEffectSolver *rows = qtt_effect_solver_new(arena);
    QttEffectConstraintSet *constraints = qtt_effect_constraints_new();
    assert(arena && rows && constraints);

    QttEffectRow *empty = qtt_effect_empty(arena);
    QttEffectRow *io = qtt_effect_extend(
        arena, "io", qtt_effect_empty(arena));
    QttEffectRow *state = qtt_effect_extend(
        arena, "state", qtt_effect_empty(arena));
    QttEffectRow *io_state = qtt_effect_extend(
        arena, "io", qtt_effect_extend(
            arena, "state", qtt_effect_empty(arena)));
    QttEffectRow *open_io = qtt_effect_extend(
        arena, "io", qtt_effect_fresh(arena));
    QttEffectRow *inferred_super = qtt_effect_extend(
        arena, "io", qtt_effect_fresh(arena));
    QttEffectRow *allocate = qtt_effect_extend(
        arena, "alloc", qtt_effect_empty(arena));
    QttEffectRow *join_result = qtt_effect_fresh(arena);
    assert(empty && io && state && io_state && open_io &&
           inferred_super && allocate && join_result);

    assert(qtt_effect_constrain_subrow(constraints, empty, io));
    assert(qtt_effect_constrain_subrow(constraints, io, io_state));
    assert(qtt_effect_constrain_join(
        constraints, io, state, io_state));
    assert(qtt_effect_constrain_equal(constraints, io, io));
    assert(qtt_effect_constrain_subrow(constraints, open_io, io_state));
    assert(qtt_effect_constrain_subrow(
        constraints, io_state, inferred_super));
    assert(qtt_effect_constrain_subrow(
        constraints, allocate, inferred_super));
    assert(qtt_effect_constrain_join(
        constraints, io, state, join_result));

    QttEffectConstraintCertificate *certificate = NULL;
    assert(qtt_effect_constraints_solve(
               constraints, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_RESIDUAL);
    assert(certificate && qtt_effect_certificate_count(certificate) == 8);
    assert(qtt_effect_certificate_status(certificate, 0) ==
           QTT_EFFECT_OBLIGATION_PROVED);
    assert(qtt_effect_certificate_status(certificate, 1) ==
           QTT_EFFECT_OBLIGATION_PROVED);
    assert(qtt_effect_certificate_status(certificate, 2) ==
           QTT_EFFECT_OBLIGATION_PROVED);
    assert(qtt_effect_certificate_status(certificate, 3) ==
           QTT_EFFECT_OBLIGATION_PROVED);
    assert(qtt_effect_certificate_status(certificate, 4) ==
           QTT_EFFECT_OBLIGATION_RESIDUAL);
    assert(qtt_effect_certificate_status(certificate, 5) ==
           QTT_EFFECT_OBLIGATION_PROVED);
    assert(qtt_effect_certificate_status(certificate, 6) ==
           QTT_EFFECT_OBLIGATION_PROVED);
    assert(qtt_effect_certificate_status(certificate, 7) ==
           QTT_EFFECT_OBLIGATION_PROVED);
    assert(qtt_effect_label_count(rows, inferred_super, "io") == 1);
    assert(qtt_effect_label_count(rows, inferred_super, "state") == 1);
    assert(qtt_effect_label_count(rows, inferred_super, "alloc") == 1);
    assert(qtt_effect_rows_equal(rows, join_result, io_state));
    assert(qtt_effect_subrow(rows, open_io, open_io) ==
           QTT_EFFECT_RELATION_PROVED);
    assert(qtt_effect_certificate_verify(certificate, arena, rows));
    qtt_effect_certificate_free(certificate);

    /* Negative row knowledge is constructive: absence is proved only for a
     * closed row, refuted by a known occurrence, and residual over a tail. */
    QttEffectConstraintSet *lacks = qtt_effect_constraints_new();
    assert(lacks);
    assert(qtt_effect_constrain_lacks(lacks, io, "state"));
    assert(qtt_effect_constraints_solve(
               lacks, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_SOLVED);
    assert(qtt_effect_certificate_status(certificate, 0) ==
           QTT_EFFECT_OBLIGATION_PROVED);
    assert(qtt_effect_certificate_verify(certificate, arena, rows));
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(lacks);

    lacks = qtt_effect_constraints_new();
    assert(lacks && qtt_effect_constrain_lacks(lacks, io, "io"));
    assert(qtt_effect_constraints_solve(
               lacks, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_REJECTED);
    assert(qtt_effect_certificate_status(certificate, 0) ==
           QTT_EFFECT_OBLIGATION_REFUTED);
    assert(!qtt_effect_certificate_verify(certificate, arena, rows));
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(lacks);

    QttEffectRow *lacks_tail = qtt_effect_fresh(arena);
    QttEffectRow *open_lacks = qtt_effect_extend(arena, "io", lacks_tail);
    lacks = qtt_effect_constraints_new();
    assert(lacks && open_lacks);
    assert(qtt_effect_constrain_lacks(lacks, open_lacks, "state"));
    assert(qtt_effect_constraints_solve(
               lacks, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_RESIDUAL);
    assert(qtt_effect_certificate_status(certificate, 0) ==
           QTT_EFFECT_OBLIGATION_RESIDUAL);
    char *lacks_text = qtt_effect_certificate_format(certificate, rows);
    assert(lacks_text && strstr(lacks_text, "lacks residual") != NULL);
    assert(strstr(lacks_text, ": state") != NULL);
    free(lacks_text);
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(lacks);

    lacks = qtt_effect_constraints_new();
    assert(lacks && qtt_effect_constrain_lacks(
        lacks, open_lacks, "state"));
    assert(qtt_effect_constrain_equal(lacks, lacks_tail, empty));
    assert(qtt_effect_constraints_solve(
               lacks, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_SOLVED);
    assert(qtt_effect_certificate_status(certificate, 0) ==
           QTT_EFFECT_OBLIGATION_PROVED);
    assert(qtt_effect_certificate_verify(certificate, arena, rows));
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(lacks);

    /* Reversing independent lower bounds preserves the principal known
     * component; only fresh tail identity may differ. */
    QttEffectRow *reverse_super = qtt_effect_extend(
        arena, "io", qtt_effect_fresh(arena));
    QttEffectConstraintSet *reverse = qtt_effect_constraints_new();
    assert(reverse_super && reverse);
    assert(qtt_effect_constrain_subrow(reverse, allocate, reverse_super));
    assert(qtt_effect_constrain_subrow(reverse, io_state, reverse_super));
    assert(qtt_effect_constraints_solve(
               reverse, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_SOLVED);
    assert(qtt_effect_label_count(rows, reverse_super, "io") == 1);
    assert(qtt_effect_label_count(rows, reverse_super, "state") == 1);
    assert(qtt_effect_label_count(rows, reverse_super, "alloc") == 1);
    assert(qtt_effect_certificate_verify(certificate, arena, rows));
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(reverse);

    QttEffectRow *dependent = qtt_effect_extend(
        arena, "io", qtt_effect_fresh(arena));
    QttEffectConstraintSet *ordered = qtt_effect_constraints_new();
    assert(dependent && ordered);
    assert(qtt_effect_constrain_subrow(ordered, dependent, io_state));
    assert(qtt_effect_constrain_equal(ordered, dependent, io_state));
    assert(qtt_effect_constraints_solve(
               ordered, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_SOLVED);
    assert(qtt_effect_certificate_status(certificate, 0) ==
           QTT_EFFECT_OBLIGATION_PROVED);
    assert(qtt_effect_certificate_verify(certificate, arena, rows));
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(ordered);

    QttEffectRow *open_lower = qtt_effect_extend(
        arena, "network", qtt_effect_fresh(arena));
    QttEffectRow *open_upper = qtt_effect_extend(
        arena, "io", qtt_effect_fresh(arena));
    QttEffectConstraintSet *residual_graph =
        qtt_effect_constraints_new();
    assert(open_lower && open_upper && residual_graph);
    assert(qtt_effect_constrain_subrow(
        residual_graph, open_lower, open_upper));
    assert(qtt_effect_constrain_subrow(
        residual_graph, open_lower, open_upper));
    assert(qtt_effect_constraints_solve(
               residual_graph, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_RESIDUAL);
    assert(qtt_effect_label_count(rows, open_upper, "io") == 1);
    assert(qtt_effect_label_count(rows, open_upper, "network") == 1);
    assert(qtt_effect_certificate_count(certificate) == 1);
    assert(qtt_effect_certificate_status(certificate, 0) ==
           QTT_EFFECT_OBLIGATION_RESIDUAL);
    assert(qtt_effect_certificate_verify(certificate, arena, rows));
    uint64_t residual_fingerprint = qtt_effect_certificate_fingerprint(
        certificate, rows);
    char *residual_text = qtt_effect_certificate_format(
        certificate, rows);
    assert(residual_fingerprint != 0 && residual_text);
    assert(strncmp(residual_text, "monad-effect-certificate-v1\n", 28) == 0);
    assert(strstr(residual_text, "e0") != NULL);
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(residual_graph);

    (void)qtt_effect_fresh(arena);
    (void)qtt_effect_fresh(arena);
    QttEffectRow *renamed_lower = qtt_effect_extend(
        arena, "network", qtt_effect_fresh(arena));
    QttEffectRow *renamed_upper = qtt_effect_extend(
        arena, "io", qtt_effect_fresh(arena));
    QttEffectConstraintSet *renamed_graph =
        qtt_effect_constraints_new();
    assert(renamed_lower && renamed_upper && renamed_graph);
    assert(qtt_effect_constrain_subrow(
        renamed_graph, renamed_lower, renamed_upper));
    assert(qtt_effect_constraints_solve(
               renamed_graph, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_RESIDUAL);
    char *renamed_text = qtt_effect_certificate_format(certificate, rows);
    assert(renamed_text && strcmp(residual_text, renamed_text) == 0);
    assert(qtt_effect_certificate_fingerprint(certificate, rows) ==
           residual_fingerprint);
    free(renamed_text);
    free(residual_text);
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(renamed_graph);

    QttEffectRow *order_a_lower = qtt_effect_extend(
        arena, "network", qtt_effect_fresh(arena));
    QttEffectRow *order_a_upper = qtt_effect_extend(
        arena, "io", qtt_effect_fresh(arena));
    QttEffectRow *order_b_lower = qtt_effect_extend(
        arena, "alloc", qtt_effect_fresh(arena));
    QttEffectRow *order_b_upper = qtt_effect_extend(
        arena, "state", qtt_effect_fresh(arena));
    QttEffectConstraintSet *forward = qtt_effect_constraints_new();
    assert(forward && qtt_effect_constrain_subrow(
        forward, order_a_lower, order_a_upper));
    assert(qtt_effect_constrain_subrow(
        forward, order_b_lower, order_b_upper));
    assert(qtt_effect_constraints_solve(
               forward, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_RESIDUAL);
    char *forward_text = qtt_effect_certificate_format(certificate, rows);
    uint64_t forward_id = qtt_effect_certificate_fingerprint(
        certificate, rows);
    assert(forward_text && forward_id);
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(forward);

    QttEffectRow *reverse_a_lower = qtt_effect_extend(
        arena, "network", qtt_effect_fresh(arena));
    QttEffectRow *reverse_a_upper = qtt_effect_extend(
        arena, "io", qtt_effect_fresh(arena));
    QttEffectRow *reverse_b_lower = qtt_effect_extend(
        arena, "alloc", qtt_effect_fresh(arena));
    QttEffectRow *reverse_b_upper = qtt_effect_extend(
        arena, "state", qtt_effect_fresh(arena));
    QttEffectConstraintSet *backward = qtt_effect_constraints_new();
    assert(backward && qtt_effect_constrain_subrow(
        backward, reverse_b_lower, reverse_b_upper));
    assert(qtt_effect_constrain_subrow(
        backward, reverse_a_lower, reverse_a_upper));
    assert(qtt_effect_constraints_solve(
               backward, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_RESIDUAL);
    char *backward_text = qtt_effect_certificate_format(certificate, rows);
    assert(backward_text && strcmp(forward_text, backward_text) == 0);
    assert(qtt_effect_certificate_fingerprint(certificate, rows) ==
           forward_id);
    free(backward_text);
    free(forward_text);
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(backward);

    QttEffectRow *cycle_a = qtt_effect_fresh(arena);
    QttEffectRow *cycle_b = qtt_effect_fresh(arena);
    QttEffectRow *cycle_c = qtt_effect_fresh(arena);
    QttEffectConstraintSet *cycle = qtt_effect_constraints_new();
    assert(cycle_a && cycle_b && cycle_c && cycle);
    assert(qtt_effect_constrain_subrow(cycle, cycle_a, cycle_b));
    assert(qtt_effect_constrain_subrow(cycle, cycle_b, cycle_c));
    assert(qtt_effect_constrain_subrow(cycle, cycle_c, cycle_a));
    assert(qtt_effect_constraints_solve(
               cycle, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_SOLVED);
    assert(qtt_effect_rows_equal(rows, cycle_a, cycle_b));
    assert(qtt_effect_rows_equal(rows, cycle_b, cycle_c));
    assert(qtt_effect_certificate_verify(certificate, arena, rows));
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(cycle);

    QttEffectConstraintSet *rejected = qtt_effect_constraints_new();
    assert(rejected && qtt_effect_constrain_subrow(
        rejected, io_state, io));
    assert(qtt_effect_constraints_solve(
               rejected, arena, rows, &certificate) ==
           QTT_EFFECT_CONSTRAINT_REJECTED);
    assert(certificate && !qtt_effect_certificate_verify(
        certificate, arena, rows));

    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(rejected);
    qtt_effect_constraints_free(constraints);
    qtt_effect_solver_free(rows);
    qtt_effect_arena_free(arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            harness = Path(directory) / "effect_constraints.c"
            executable = Path(directory) / "effect_constraints"
            harness.write_text(source)
            subprocess.run([
                "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-iquote", str(ROOT), str(harness),
                str(ROOT / "effects" / "effect.c"),
                str(ROOT / "effects" / "constraints.c"),
                str(ROOT / "qtt" / "place.c"),
                str(ROOT / "qtt" / "quantity.c"),
                "-o", str(executable),
            ], cwd=ROOT, check=True)
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
