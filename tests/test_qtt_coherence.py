"""Executable agreement boundary between legacy and symbolic QTT semantics."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttCoherenceTests(unittest.TestCase):
    def test_common_fragment_agrees_and_higher_order_is_explicitly_excluded(self):
        source = r'''
#include "qtt/coherence.h"
#include "qtt/demand.h"
#include "qtt/semantic_ir.h"
#include <assert.h>

int main(void) {
    QttCoreVar x = {.module_id = 7, .binder_id = 11};
    QttCoreNode x1 = {.kind = QTT_CORE_VAR, .var = x};
    QttCoreNode x2 = {.kind = QTT_CORE_VAR, .var = x};
    QttCoreNode x3 = {.kind = QTT_CORE_VAR, .var = x};
    QttCoreNode condition = {
        .kind = QTT_CORE_GLOBAL, .global = {.name = "condition"}};
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
    QttCoreVar subjects[] = {x};
    QttCoherenceReport report =
        qtt_coherence_check(&branch, subjects, 1);
    assert(report.status == QTT_COHERENCE_AGREES);
    assert(report.checked_binder_count == 1);
    assert(report.legacy.finite == 2);
    assert(report.symbolic.finite == 2);
    assert(report.boundary == QTT_COHERENCE_BOUNDARY_NONE);

    QttCoreVar y = {.module_id = 7, .binder_id = 12};
    QttCoreNode binding = {.kind = QTT_CORE_VAR, .var = y};
    QttCoreNode bound1 = {.kind = QTT_CORE_VAR, .var = x};
    QttCoreNode bound2 = {.kind = QTT_CORE_VAR, .var = x};
    QttCoreNode *bound_twice_items[] = {&bound1, &bound2};
    QttCoreNode bound_twice = {
        .kind = QTT_CORE_SEQUENCE,
        .sequence = {.items = bound_twice_items, .count = 2},
    };
    QttCoreNode let = {
        .kind = QTT_CORE_LET,
        .let = {.binding = x, .value = &binding, .body = &bound_twice},
    };
    QttCoreVar let_subjects[] = {x, y};
    report = qtt_coherence_check(&let, let_subjects, 2);
    assert(report.status == QTT_COHERENCE_AGREES);
    assert(report.checked_binder_count == 2);
    assert(report.legacy.finite == 2);
    assert(report.symbolic.finite == 2);
    QttDemandError demand_error = QTT_DEMAND_OK;
    QttDemandCertificate *let_certificate =
        qtt_demand_derive(&let, &demand_error);
    QttDemandDerivation *let_proof =
        qtt_demand_prove(let_certificate, &let, y, &demand_error);
    assert(let_proof && let_proof->premise_count == 3);
    assert(qtt_core_var_equal(
        let_proof->premises[2]->subject, x));
    assert(let_proof->premises[2]->runtime.finite == 2);
    assert(qtt_demand_check(let_proof, &let, y) ==
           QTT_DEMAND_PROOF_VALID);
    let_proof->premises[2]->runtime = qtt_quantity_finite(1);
    assert(qtt_demand_check(let_proof, &let, y) ==
           QTT_DEMAND_PROOF_GRADE_MISMATCH);
    qtt_demand_derivation_free(let_proof);
    qtt_demand_certificate_free(let_certificate);

    QttCoreNode unused = {.kind = QTT_CORE_GLOBAL,
                          .global = {.name = "unused"}};
    let.let.body = &unused;
    report = qtt_coherence_check(&let, let_subjects, 2);
    assert(report.status == QTT_COHERENCE_AGREES);
    assert(report.legacy.finite == 0);
    assert(report.symbolic.finite == 0);

    QttCoreNode captured1 = {.kind = QTT_CORE_VAR, .var = y};
    QttCoreNode captured2 = {.kind = QTT_CORE_VAR, .var = y};
    QttCoreNode *captured_items[] = {&captured1, &captured2};
    QttCoreNode captured_twice = {
        .kind = QTT_CORE_SEQUENCE,
        .sequence = {.items = captured_items, .count = 2},
    };
    QttCoreNode lambda = {
        .kind = QTT_CORE_LAMBDA,
        .lambda = {.params = &x, .param_count = 1, .body = &captured_twice},
    };
    report = qtt_coherence_check(&lambda, let_subjects, 2);
    assert(report.status == QTT_COHERENCE_AGREES);
    assert(report.checked_binder_count == 2);
    assert(report.legacy.finite == 1);
    assert(report.symbolic.finite == 1);

    QttCoreVar z = {.module_id = 7, .binder_id = 13};
    QttCoreNode parameter1 = {.kind = QTT_CORE_VAR, .var = x};
    QttCoreNode parameter2 = {.kind = QTT_CORE_VAR, .var = x};
    QttCoreNode capture1 = {.kind = QTT_CORE_VAR, .var = z};
    QttCoreNode capture2 = {.kind = QTT_CORE_VAR, .var = z};
    QttCoreNode *call_body_items[] = {
        &parameter1, &parameter2, &capture1, &capture2};
    QttCoreNode call_body = {
        .kind = QTT_CORE_SEQUENCE,
        .sequence = {.items = call_body_items, .count = 4},
    };
    lambda.lambda.body = &call_body;
    QttCoreNode argument = {.kind = QTT_CORE_VAR, .var = y};
    QttCoreNode *arguments[] = {&argument};
    QttCoreNode apply = {
        .kind = QTT_CORE_APPLY,
        .apply = {
            .callee = &lambda,
            .arguments = arguments,
            .argument_count = 1,
        },
    };
    QttCoreVar call_subjects[] = {y, z};
    report = qtt_coherence_check(&apply, call_subjects, 2);
    assert(report.status == QTT_COHERENCE_AGREES);
    assert(report.checked_binder_count == 2);
    /* z is captured once and demanded twice by the single invocation. */
    assert(report.legacy.finite == 3);
    assert(report.symbolic.finite == 3);
    QttDemandCertificate *call_certificate =
        qtt_demand_derive(&apply, &demand_error);
    QttDemandDerivation *call_proof =
        qtt_demand_prove(call_certificate, &apply, y, &demand_error);
    assert(call_proof && call_proof->premise_count == 4);
    assert(qtt_core_var_equal(call_proof->premises[3]->subject, x));
    assert(call_proof->premises[3]->runtime.finite == 2);
    assert(qtt_demand_check(call_proof, &apply, y) ==
           QTT_DEMAND_PROOF_VALID);
    call_proof->premises[3]->runtime = qtt_quantity_finite(1);
    assert(qtt_demand_check(call_proof, &apply, y) ==
           QTT_DEMAND_PROOF_GRADE_MISMATCH);
    qtt_demand_derivation_free(call_proof);
    qtt_demand_certificate_free(call_certificate);

    QttCoreNode unknown = {
        .kind = QTT_CORE_GLOBAL, .global = {.name = "unknown"}};
    apply.apply.callee = &unknown;
    report = qtt_coherence_check(&apply, call_subjects, 2);
    assert(report.status == QTT_COHERENCE_OUTSIDE_COMMON_FRAGMENT);
    assert(report.boundary == QTT_COHERENCE_BOUNDARY_APPLICATION);

    QttCoreVar foreign = {.module_id = 8, .binder_id = 11};
    QttCoreNode foreign_use = {.kind = QTT_CORE_VAR, .var = foreign};
    QttCoreNode *mixed_items[] = {&x1, &foreign_use};
    QttCoreNode mixed = {
        .kind = QTT_CORE_SEQUENCE,
        .sequence = {.items = mixed_items, .count = 2},
    };
    report = qtt_coherence_check(&mixed, subjects, 1);
    assert(report.status == QTT_COHERENCE_INVALID_INPUT);

    Type int_type = {.kind = TYPE_INT};
    QttCoreNode typed_x1 = {
        .kind = QTT_CORE_VAR, .type = &int_type, .var = x};
    QttCoreNode typed_x2 = {
        .kind = QTT_CORE_VAR, .type = &int_type, .var = x};
    QttCoreNode *typed_items[] = {&typed_x1, &typed_x2};
    QttCoreNode typed_sequence = {
        .kind = QTT_CORE_SEQUENCE, .type = &int_type,
        .sequence = {.items = typed_items, .count = 2}};
    QttSemanticIrError ir_error = QTT_SEMANTIC_IR_OK;
    QttSemanticFunction *semantic =
        qtt_semantic_ir_lower(&typed_sequence, &ir_error);
    assert(semantic && ir_error == QTT_SEMANTIC_IR_OK);
    report = qtt_coherence_check_semantic(
        semantic, &typed_sequence, subjects, 1);
    assert(report.status == QTT_COHERENCE_AGREES);
    assert(report.legacy.finite == 2);
    assert(report.symbolic.finite == 2);
    QttCoreVar wrong_module = {
        .module_id = 99, .binder_id = x.binder_id};
    report = qtt_coherence_check_semantic(
        semantic, &typed_sequence, &wrong_module, 1);
    assert(report.status == QTT_COHERENCE_INVALID_INPUT);
    QttSemanticNode *semantic_nodes =
        qtt_semantic_ir_nodes(semantic);
    semantic_nodes[0].capability =
        QTT_SEMANTIC_CAPABILITY_OWNED;
    report = qtt_coherence_check_semantic(
        semantic, &typed_sequence, subjects, 1);
    assert(report.status == QTT_COHERENCE_INVALID_INPUT);
    qtt_semantic_ir_free(semantic);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_coherence_test.c"
            executable = directory / "qtt_coherence_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "core.c"),
                    str(ROOT / "qtt" / "quantity.c"),
                    str(ROOT / "qtt" / "demand.c"),
                    str(ROOT / "qtt" / "graded.c"),
                    str(ROOT / "qtt" / "constraints.c"),
                    str(ROOT / "qtt" / "environment.c"),
                    str(ROOT / "qtt" / "elaboration.c"),
                    str(ROOT / "qtt" / "core_usage.c"),
                    str(ROOT / "qtt" / "type_identity.c"),
                    str(ROOT / "qtt" / "signature.c"),
                    str(ROOT / "qtt" / "signature_env.c"),
                    str(ROOT / "qtt" / "call.c"),
                    str(ROOT / "effects" / "effect.c"),
                    str(ROOT / "qtt" / "place.c"),
                    str(ROOT / "qtt" / "resource.c"),
                    str(ROOT / "qtt" / "drop.c"),
                    str(ROOT / "qtt" / "closure_policy.c"),
                    str(ROOT / "qtt" / "semantic_ir.c"),
                    str(ROOT / "qtt" / "coherence.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    unittest.main()
