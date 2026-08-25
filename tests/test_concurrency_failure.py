"""Cycle 3.2 portable causal failure certificates."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ConcurrencyFailureCertificateTests(unittest.TestCase):
    def test_failure_certificate_conserves_linear_causes(self):
        source = r'''
#include "concurrency/failure.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static ConcurrencyTaskId task(uint64_t id) {
    return (ConcurrencyTaskId){.module_id = 11, .task_id = id};
}

int main(void) {
    ConcurrencyStep steps[] = {
        concurrency_spawn(task(1), task(2), NULL, 0),
        concurrency_spawn(task(2), task(3), NULL, 0),
        concurrency_fail(task(3)),
        concurrency_cleanup_fail(task(3)),
        concurrency_cleanup_complete(task(3)),
        concurrency_join(task(2), task(3)),
        concurrency_fail(task(2)),
        concurrency_cleanup_complete(task(2)),
        concurrency_join(task(1), task(2)),
    };
    ConcurrencyTrace trace = {
        .root = task(1),
        .steps = steps,
        .step_count = sizeof(steps) / sizeof(steps[0]),
    };
    QttEffectArena *arena = qtt_effect_arena_new();
    assert(arena);
    QttEffectRow *row = qtt_effect_empty(arena);
    for (int operation = CONCURRENCY_EFFECT_JOIN;
         operation >= CONCURRENCY_EFFECT_SPAWN; operation--) {
        QttEffectAtom atom = concurrency_effect_atom(
            (ConcurrencyEffectOperation)operation, 91, 92);
        row = qtt_effect_extend_atom(arena, &atom, row);
    }
    QttEffectSolver *solver = qtt_effect_solver_new(arena);
    assert(solver);
    ConcurrencyFailureAuthority authority = {
        .solver = solver,
        .effects = row,
        .scope_capability_id = 91,
        .task_type_id = 92,
    };
    ConcurrencyFailureCertificate certificate = {0};
    assert(concurrency_failure_build(&trace, &authority, &certificate) ==
           CONCURRENCY_FAILURE_CERTIFIED);
    assert(certificate.node_count == 3);
    assert(certificate.root_count == 1);
    assert(certificate.total_grade.finite == 3);
    assert(!certificate.total_grade.is_omega);
    assert(concurrency_failure_verify(&trace, &authority, &certificate) ==
           CONCURRENCY_FAILURE_CERTIFIED);
    assert(certificate.effect_fingerprint ==
           qtt_effect_row_fingerprint(solver, row));

    size_t encoded_size = concurrency_failure_encoded_size(&certificate);
    assert(encoded_size > 0);
    uint8_t *encoded = malloc(encoded_size);
    assert(encoded);
    size_t written = 0;
    assert(concurrency_failure_encode(
               &certificate, encoded, encoded_size, &written) ==
           CONCURRENCY_FAILURE_CERTIFIED);
    assert(written == encoded_size);
    assert(memcmp(encoded, "MCFC", 4) == 0);
    assert(encoded[4] == 0 && encoded[5] == 1);

    ConcurrencyFailureCertificate decoded = {0};
    assert(concurrency_failure_decode(
               encoded, encoded_size, &decoded) ==
           CONCURRENCY_FAILURE_CERTIFIED);
    assert(concurrency_failure_verify(&trace, &authority, &decoded) ==
           CONCURRENCY_FAILURE_CERTIFIED);
    concurrency_failure_free(&decoded);

    assert(concurrency_failure_decode(
               encoded, encoded_size - 1, &decoded) ==
           CONCURRENCY_FAILURE_TRUNCATED);
    encoded[5] = 2;
    assert(concurrency_failure_decode(
               encoded, encoded_size, &decoded) ==
           CONCURRENCY_FAILURE_UNSUPPORTED_VERSION);
    encoded[5] = 1;

    for (size_t i = 0; i < encoded_size; i++) {
        encoded[i] ^= 0xa5;
        ConcurrencyFailureCertificate mutated = {0};
        ConcurrencyFailureCertificateStatus mutation =
            concurrency_failure_decode(encoded, encoded_size, &mutated);
        if (mutation == CONCURRENCY_FAILURE_CERTIFIED)
            assert(concurrency_failure_verify(
                       &trace, &authority, &mutated) !=
                   CONCURRENCY_FAILURE_CERTIFIED);
        concurrency_failure_free(&mutated);
        encoded[i] ^= 0xa5;
    }
    free(encoded);

    QttEffectAtom spawn = concurrency_effect_atom(
        CONCURRENCY_EFFECT_SPAWN, 91, 92);
    QttEffectRow *insufficient = qtt_effect_extend_atom(
        arena, &spawn, qtt_effect_empty(arena));
    ConcurrencyFailureAuthority missing_effect = authority;
    missing_effect.effects = insufficient;
    ConcurrencyFailureCertificate rejected = {0};
    assert(concurrency_failure_build(
               &trace, &missing_effect, &rejected) ==
           CONCURRENCY_FAILURE_INVALID_TRACE);

    size_t child = certificate.node_count;
    size_t cleanup = certificate.node_count;
    for (size_t i = 0; i < certificate.node_count; i++) {
        if (certificate.nodes[i].task.task_id == 3 &&
            certificate.nodes[i].cause == CONCURRENCY_FAILURE_TASK)
            child = i;
        if (certificate.nodes[i].cause == CONCURRENCY_FAILURE_CLEANUP)
            cleanup = i;
    }
    assert(child < certificate.node_count);
    assert(cleanup < certificate.node_count);
    assert(certificate.nodes[child].parent_index < certificate.node_count);
    assert(certificate.nodes[certificate.nodes[child].parent_index]
               .task.task_id == 2);
    assert(certificate.nodes[cleanup].parent_index == child);

    uint64_t fingerprint = certificate.fingerprint;
    certificate.nodes[cleanup].grade = qtt_quantity_omega();
    assert(concurrency_failure_verify(&trace, &authority, &certificate) ==
           CONCURRENCY_FAILURE_INVALID_GRADE);
    certificate.nodes[cleanup].grade = qtt_quantity_finite(1);
    certificate.nodes[cleanup].parent_index = SIZE_MAX;
    certificate.fingerprint = fingerprint;
    assert(concurrency_failure_verify(&trace, &authority, &certificate) !=
           CONCURRENCY_FAILURE_CERTIFIED);

    concurrency_failure_free(&certificate);
    qtt_effect_solver_free(solver);
    qtt_effect_arena_free(arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "failure_test.c"
            binary_path = Path(directory) / "failure_test"
            source_path.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(source_path),
                    str(ROOT / "concurrency" / "kernel.c"),
                    str(ROOT / "concurrency" / "failure.c"),
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
