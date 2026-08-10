"""Quantities constrain type validity rather than merely describing demand."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttGradedTypingTests(unittest.TestCase):
    def test_declared_subusage_is_checked_and_certified(self):
        source = r'''
#include "qtt/graded.h"
#include <assert.h>

int main(void) {
    assert(qtt_quantity_leq(qtt_quantity_finite(0),
                            qtt_quantity_finite(1)));
    assert(qtt_quantity_leq(qtt_quantity_finite(7),
                            qtt_quantity_omega()));
    assert(!qtt_quantity_leq(qtt_quantity_finite(2),
                             qtt_quantity_finite(1)));
    assert(!qtt_quantity_leq(qtt_quantity_omega(),
                             qtt_quantity_finite(UINT64_MAX)));

    Type string_type = {.kind = TYPE_STRING};
    QttTypeArena *types = qtt_type_arena_new();
    QttTypeIdentityError identity_error = QTT_TYPE_IDENTITY_OK;
    QttTypeId string_id =
        qtt_type_intern(types, &string_type, &identity_error);
    assert(string_id.value);

    QttCoreVar x = {.module_id = 51, .binder_id = 1};
    QttCoreNode first = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = x};
    QttCoreNode second = {
        .kind = QTT_CORE_VAR, .type = &string_type, .var = x};
    QttCoreNode *items[] = {&first, &second};
    QttCoreNode sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &string_type,
        .sequence = {.items = items, .count = 2}};

    QttGradedBinder binder = {
        .var = x, .type_id = string_id,
        .allowance = qtt_quantity_finite(1)};
    QttGradedContext context = {.binders = &binder, .count = 1};
    QttGradedError error = QTT_GRADED_OK;
    QttGradedCertificate *certificate = qtt_graded_check(
        &sequence, string_id, &context, types, &error);
    assert(!certificate && error == QTT_GRADED_USAGE_EXCEEDED);

    binder.allowance = qtt_quantity_finite(2);
    certificate = qtt_graded_check(
        &sequence, string_id, &context, types, &error);
    assert(certificate && error == QTT_GRADED_OK);
    assert(qtt_graded_observed(certificate, x).finite == 2);
    assert(qtt_graded_validate(
        certificate, &sequence, string_id, &context, types) ==
        QTT_GRADED_PROOF_VALID);

    binder.allowance = qtt_quantity_finite(1);
    assert(qtt_graded_validate(
        certificate, &sequence, string_id, &context, types) ==
        QTT_GRADED_PROOF_CONTEXT_MISMATCH);
    binder.allowance = qtt_quantity_finite(2);

    QttTypeId wrong = {.value = string_id.value + 1};
    assert(!qtt_graded_check(
        &sequence, wrong, &context, types, &error));
    assert(error == QTT_GRADED_TYPE_MISMATCH);

    QttCoreNode unused = {
        .kind = QTT_CORE_GLOBAL, .type = &string_type,
        .global = {.name = "constant"}};
    binder.allowance = qtt_quantity_finite(0);
    QttGradedCertificate *erased = qtt_graded_check(
        &unused, string_id, &context, types, &error);
    assert(erased && qtt_graded_is_erased(erased, x));

    qtt_graded_certificate_free(erased);
    qtt_graded_certificate_free(certificate);
    qtt_type_arena_free(types);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_graded_test.c"
            executable = directory / "qtt_graded_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "core.c"),
                    str(ROOT / "qtt" / "quantity.c"),
                    str(ROOT / "qtt" / "demand.c"),
                    str(ROOT / "qtt" / "type_identity.c"),
                    str(ROOT / "qtt" / "graded.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
