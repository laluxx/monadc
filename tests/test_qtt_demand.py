"""Canonical quantitative derivations over typed Core."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttDemandTests(unittest.TestCase):
    def test_branch_derivation_is_checkable_and_path_sensitive(self):
        source = r'''
#include "qtt/demand.h"
#include <assert.h>
#include <string.h>

int main(void) {
    QttCoreVar x = {.module_id = 3, .binder_id = 9};
    QttCoreNode condition = {
        .kind = QTT_CORE_GLOBAL, .global = {.name = "True"}};
    QttCoreNode x1 = {.kind = QTT_CORE_VAR, .var = x};
    QttCoreNode x2 = {.kind = QTT_CORE_VAR, .var = x};
    QttCoreNode x3 = {.kind = QTT_CORE_VAR, .var = x};
    QttCoreNode *twice_items[] = {&x2, &x3};
    QttCoreNode twice = {
        .kind = QTT_CORE_SEQUENCE,
        .sequence = {.items = twice_items, .count = 2},
    };
    QttCoreNode branch = {
        .kind = QTT_CORE_IF,
        .conditional = {
            .condition = &condition,
            .then_branch = &x1,
            .else_branch = &twice,
        },
    };

    QttDemandError error = QTT_DEMAND_OK;
    QttDemandCertificate *certificate =
        qtt_demand_derive(&branch, &error);
    assert(certificate && error == QTT_DEMAND_OK);
    assert(qtt_demand_validate(certificate, &branch) == QTT_DEMAND_VALID);
    assert(strcmp(qtt_demand_format(certificate, &branch, x),
                  "choice(1,2)") == 0);
    assert(qtt_demand_occurrences(certificate, &x1, x) == 1);
    assert(qtt_demand_occurrences(certificate, &twice, x) == 2);
    assert(qtt_demand_runtime(certificate, &branch, x).finite == 2);

    QttDemandDerivation *proof =
        qtt_demand_prove(certificate, &branch, x, &error);
    assert(proof && error == QTT_DEMAND_OK);
    assert(proof->rule == QTT_DEMAND_RULE_IF);
    assert(proof->premise_count == 3);
    assert(proof->syntactic.finite == 3);
    assert(proof->runtime.finite == 2);
    assert(qtt_demand_check(proof, &branch, x) ==
           QTT_DEMAND_PROOF_VALID);
    proof->runtime = qtt_quantity_finite(99);
    assert(qtt_demand_check(proof, &branch, x) ==
           QTT_DEMAND_PROOF_GRADE_MISMATCH);
    proof->runtime = qtt_quantity_finite(2);
    assert(qtt_demand_check(proof, &branch, x) ==
           QTT_DEMAND_PROOF_VALID);
    qtt_demand_derivation_free(proof);

    QttCoreNode replacement = {
        .kind = QTT_CORE_GLOBAL, .global = {.name = "replacement"}};
    QttCoreNode write = {
        .kind = QTT_CORE_WRITE,
        .write = {
            .place = {.root = x},
            .value = &replacement,
        },
    };
    QttDemandCertificate *write_certificate =
        qtt_demand_derive(&write, &error);
    assert(write_certificate && error == QTT_DEMAND_OK);
    assert(qtt_demand_occurrences(
               write_certificate, &write, x) == 1);
    assert(qtt_demand_runtime(
               write_certificate, &write, x).finite == 1);
    proof = qtt_demand_prove(
        write_certificate, &write, x, &error);
    assert(proof && proof->rule == QTT_DEMAND_RULE_WRITE);
    assert(proof->premise_count == 1);
    assert(proof->syntactic.finite == 1);
    assert(proof->runtime.finite == 1);
    assert(qtt_demand_check(proof, &write, x) ==
           QTT_DEMAND_PROOF_VALID);
    qtt_demand_derivation_free(proof);
    qtt_demand_certificate_free(write_certificate);

    QttCoreNode changed = branch;
    changed.conditional.else_branch = &x2;
    assert(qtt_demand_validate(certificate, &changed) ==
           QTT_DEMAND_SOURCE_MISMATCH);
    qtt_demand_certificate_free(certificate);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_demand_test.c"
            executable = directory / "qtt_demand_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "core.c"),
                    str(ROOT / "qtt" / "quantity.c"),
                    str(ROOT / "qtt" / "demand.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
