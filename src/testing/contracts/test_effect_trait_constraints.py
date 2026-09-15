"""Three-valued HasTrait constraints over closed and open effect rows."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class EffectTraitConstraintTests(unittest.TestCase):
    def test_proved_refuted_and_residual_judgments(self):
        source = r'''
#include "effects/constraints.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    assert(qtt_effect_trait_implication_register("allocate", "state"));
    assert(qtt_effect_trait_implication_register("state", "io"));
    QttEffectArena *arena = qtt_effect_arena_new();
    QttEffectSolver *solver = qtt_effect_solver_new(arena);
    QttEffectAtom allocation = {.traits = "allocate",
        .constructor_id = 1, .operation = "heap",
        .resumption = {.finite = 1}};
    QttEffectRow *closed = qtt_effect_extend_atom(
        arena, &allocation, qtt_effect_empty(arena));
    QttEffectRow *open = qtt_effect_extend_atom(
        arena, &allocation, qtt_effect_fresh(arena));

    QttEffectConstraintSet *proved = qtt_effect_constraints_new();
    assert(qtt_effect_constrain_has_trait(proved, closed, "io"));
    QttEffectConstraintCertificate *certificate = NULL;
    assert(qtt_effect_constraints_solve(
        proved, arena, solver, &certificate) == QTT_EFFECT_CONSTRAINT_SOLVED);
    assert(qtt_effect_certificate_status(certificate, 0) ==
           QTT_EFFECT_OBLIGATION_PROVED);
    char *portable = qtt_effect_certificate_format(certificate, solver);
    assert(portable && strstr(portable, "monad-effect-certificate-v2\n") == portable);
    free(portable);
    char *witness = qtt_effect_certificate_trait_witness(
        certificate, 0, solver);
    assert(witness && strcmp(witness, "allocate -> state -> io") == 0);
    free(witness); qtt_effect_certificate_free(certificate);

    QttEffectConstraintSet *refuted = qtt_effect_constraints_new();
    assert(qtt_effect_constrain_has_trait(refuted, closed, "telemetry"));
    assert(qtt_effect_constraints_solve(
        refuted, arena, solver, &certificate) == QTT_EFFECT_CONSTRAINT_REJECTED);
    assert(qtt_effect_certificate_status(certificate, 0) ==
           QTT_EFFECT_OBLIGATION_REFUTED);
    qtt_effect_certificate_free(certificate);

    QttEffectConstraintSet *residual = qtt_effect_constraints_new();
    assert(qtt_effect_constrain_has_trait(residual, open, "telemetry"));
    assert(qtt_effect_constraints_solve(
        residual, arena, solver, &certificate) == QTT_EFFECT_CONSTRAINT_RESIDUAL);
    assert(qtt_effect_certificate_status(certificate, 0) ==
           QTT_EFFECT_OBLIGATION_RESIDUAL);
    assert(qtt_effect_certificate_verify(certificate, arena, solver));
    qtt_effect_certificate_free(certificate);
    qtt_effect_constraints_free(proved);
    qtt_effect_constraints_free(refuted);
    qtt_effect_constraints_free(residual);
    qtt_effect_solver_free(solver); qtt_effect_arena_free(arena);
    qtt_effect_trait_implications_clear();
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "trait_constraints.c"
            executable = directory / "trait_constraints"
            harness.write_text(source)
            subprocess.run([
                "cc", "-std=c99", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-iquote", str(ROOT / "src"), str(harness),
                str(ROOT / "src" / "effects" / "effect.c"),
                str(ROOT / "src" / "effects" / "constraints.c"),
                str(ROOT / "src" / "qtt" / "place.c"), str(ROOT / "src" / "qtt" / "quantity.c"),
                "-o", str(executable)], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
