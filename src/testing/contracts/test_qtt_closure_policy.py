"""Conservative, independently checkable closure storage inference."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


class QttClosurePolicyTests(unittest.TestCase):
    def test_only_single_direct_application_is_unique(self):
        source = r'''
#include "qtt/closure_policy.h"
#include "qtt/signature_env.h"
#include <assert.h>

int main(void) {
    Type integer = {.kind = TYPE_INT};
    QttCoreNode body = {.kind = QTT_CORE_LITERAL, .type = &integer};
    QttCoreNode lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &integer,
        .lambda = {.body = &body},
    };
    QttCoreNode direct = {
        .kind = QTT_CORE_APPLY, .type = &integer,
        .apply = {.callee = &lambda},
    };

    QttClosurePolicyEvidence evidence =
        qtt_closure_policy_infer(&direct, &lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_UNIQUE);
    assert(evidence.reason == QTT_CLOSURE_POLICY_SINGLE_DIRECT_CALL);
    assert(evidence.occurrence_count == 1);
    assert(evidence.direct_callee_count == 1);
    assert(qtt_closure_policy_verify(&direct, &lambda, evidence));

    QttCoreVar stored_var = {.module_id = 1, .binder_id = 2};
    QttCoreNode stored_use = {
        .kind = QTT_CORE_VAR, .type = &integer, .var = stored_var};
    QttCoreNode stored_call = {
        .kind = QTT_CORE_APPLY, .type = &integer,
        .apply = {.callee = &stored_use},
    };
    QttCoreNode stored = {
        .kind = QTT_CORE_LET, .type = &integer,
        .let = {.binding = stored_var, .value = &lambda,
                .body = &stored_call},
    };
    evidence = qtt_closure_policy_infer(&stored, &lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_UNIQUE);
    assert(evidence.reason == QTT_CLOSURE_POLICY_SINGLE_DIRECT_CALL);
    assert(evidence.occurrence_count == 1);
    assert(evidence.direct_callee_count == 1);
    assert(qtt_closure_policy_verify(&stored, &lambda, evidence));

    QttCoreNode *stored_twice_items[] = {&stored_call, &stored_call};
    QttCoreNode stored_twice_body = {
        .kind = QTT_CORE_SEQUENCE, .type = &integer,
        .sequence = {.items = stored_twice_items, .count = 2},
    };
    stored.let.body = &stored_twice_body;
    evidence = qtt_closure_policy_infer(&stored, &lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_SHARED);
    assert(evidence.reason == QTT_CLOSURE_POLICY_MULTIPLE_OCCURRENCES);
    assert(evidence.occurrence_count == 2);
    assert(evidence.direct_callee_count == 2);
    stored.let.body = &stored_call;

    evidence.storage = QTT_CLOSURE_STORAGE_SHARED;
    assert(!qtt_closure_policy_verify(&direct, &lambda, evidence));
    evidence = qtt_closure_policy_infer(&direct, &lambda);
    evidence.live_after_capture_use_count = 1;
    assert(!qtt_closure_policy_verify(&direct, &lambda, evidence));

    evidence = qtt_closure_policy_infer(&lambda, &lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_SHARED);
    assert(evidence.reason == QTT_CLOSURE_POLICY_ESCAPES);

    QttCoreNode global = {
        .kind = QTT_CORE_GLOBAL, .type = &integer,
        .global = {.name = "consume"}};
    QttCoreNode *arguments[] = {&lambda};
    QttCoreNode passed = {
        .kind = QTT_CORE_APPLY, .type = &integer,
        .apply = {.callee = &global, .arguments = arguments,
                  .argument_count = 1},
    };
    evidence = qtt_closure_policy_infer(&passed, &lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_SHARED);
    assert(evidence.reason == QTT_CLOSURE_POLICY_ESCAPES);

    QttCoreVar captured = {.module_id = 1, .binder_id = 9};
    QttCoreNode capture_use = {
        .kind = QTT_CORE_VAR, .type = &integer, .var = captured};
    QttCoreNode capturing_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &integer,
        .lambda = {.body = &capture_use},
    };
    QttCoreNode capturing_call = {
        .kind = QTT_CORE_APPLY, .type = &integer,
        .apply = {.callee = &capturing_lambda},
    };
    QttCoreNode outer_use = {
        .kind = QTT_CORE_VAR, .type = &integer, .var = captured};
    QttCoreNode *live_items[] = {&capturing_call, &outer_use};
    QttCoreNode live_after = {
        .kind = QTT_CORE_SEQUENCE, .type = &integer,
        .sequence = {.items = live_items, .count = 2},
    };
    evidence =
        qtt_closure_policy_infer(&live_after, &capturing_lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_SHARED);
    assert(evidence.reason ==
           QTT_CLOSURE_POLICY_CAPTURE_USED_OUTSIDE);
    assert(evidence.external_capture_use_count == 1);
    assert(evidence.live_after_capture_use_count == 1);

    QttCoreNode *dead_items[] = {&outer_use, &capturing_call};
    QttCoreNode dead_before = {
        .kind = QTT_CORE_SEQUENCE, .type = &integer,
        .sequence = {.items = dead_items, .count = 2},
    };
    evidence =
        qtt_closure_policy_infer(&dead_before, &capturing_lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_UNIQUE);
    assert(evidence.external_capture_use_count == 1);
    assert(evidence.live_after_capture_use_count == 0);

    QttCoreNode *capture_arguments[] = {&outer_use};
    capturing_call.apply.arguments = capture_arguments;
    capturing_call.apply.argument_count = 1;
    evidence =
        qtt_closure_policy_infer(&capturing_call, &capturing_lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_SHARED);
    assert(evidence.live_after_capture_use_count == 1);
    capturing_call.apply.arguments = NULL;
    capturing_call.apply.argument_count = 0;

    QttCoreNode condition = {
        .kind = QTT_CORE_GLOBAL, .type = &integer,
        .global = {.name = "condition"}};
    QttCoreNode exclusive = {
        .kind = QTT_CORE_IF, .type = &integer,
        .conditional = {
            .condition = &condition,
            .then_branch = &capturing_call,
            .else_branch = &outer_use,
        },
    };
    evidence =
        qtt_closure_policy_infer(&exclusive, &capturing_lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_UNIQUE);
    assert(evidence.external_capture_use_count == 1);
    assert(evidence.live_after_capture_use_count == 0);

    QttCoreNode later_capture_use = {
        .kind = QTT_CORE_VAR, .type = &integer, .var = captured};
    QttCoreNode later_closure = {
        .kind = QTT_CORE_LAMBDA, .type = &integer,
        .lambda = {.body = &later_capture_use},
    };
    QttCoreNode *nested_items[] = {&capturing_call, &later_closure};
    QttCoreNode captured_later = {
        .kind = QTT_CORE_SEQUENCE, .type = &integer,
        .sequence = {.items = nested_items, .count = 2},
    };
    evidence =
        qtt_closure_policy_infer(&captured_later, &capturing_lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_SHARED);
    assert(evidence.live_after_capture_use_count == 1);

    QttCoreNode no_capture = {
        .kind = QTT_CORE_LITERAL, .type = &integer};
    QttCoreNode path_dependent_body = {
        .kind = QTT_CORE_IF, .type = &integer,
        .conditional = {
            .condition = &condition,
            .then_branch = &capture_use,
            .else_branch = &no_capture,
        },
    };
    QttCoreNode path_dependent_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &integer,
        .lambda = {.body = &path_dependent_body},
    };
    QttCoreNode path_dependent_call = {
        .kind = QTT_CORE_APPLY, .type = &integer,
        .apply = {.callee = &path_dependent_lambda},
    };
    evidence = qtt_closure_policy_infer(
        &path_dependent_call, &path_dependent_lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_SHARED);
    assert(evidence.reason ==
           QTT_CLOSURE_POLICY_INVOCATION_DISPOSITION_UNSUPPORTED);
    assert(evidence.unsupported_exit_capture_count == 1);

    QttCoreNode *items[] = {&direct, &direct};
    QttCoreNode aliased = {
        .kind = QTT_CORE_SEQUENCE, .type = &integer,
        .sequence = {.items = items, .count = 2},
    };
    evidence = qtt_closure_policy_infer(&aliased, &lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_SHARED);
    assert(evidence.reason == QTT_CLOSURE_POLICY_MULTIPLE_OCCURRENCES);
    assert(evidence.occurrence_count == 2);
    assert(evidence.direct_callee_count == 2);

    Type string = {.kind = TYPE_STRING};
    QttCoreVar heap_capture = {.module_id = 1, .binder_id = 70};
    QttCoreNode heap_use = {
        .kind = QTT_CORE_VAR, .type = &string, .var = heap_capture};
    QttCoreNode consume = {
        .kind = QTT_CORE_GLOBAL, .type = &string,
        .global = {.name = "consumeString"}};
    QttCoreNode *consume_arguments[] = {&heap_use};
    QttCoreNode consume_call = {
        .kind = QTT_CORE_APPLY, .type = &integer,
        .apply = {.callee = &consume, .arguments = consume_arguments,
                  .argument_count = 1},
    };
    QttCoreNode consuming_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &integer,
        .lambda = {.body = &consume_call},
    };
    QttCoreNode consuming_invoke = {
        .kind = QTT_CORE_APPLY, .type = &integer,
        .apply = {.callee = &consuming_lambda},
    };
    QttTypeArena *types = qtt_type_arena_new();
    assert(types);
    QttSignatureEnv *env = qtt_signature_env_new(types);
    assert(env);
    QttTypeIdentityError type_error = QTT_TYPE_IDENTITY_OK;
    QttParameterContract parameter = {
        .var = {.module_id = 1, .binder_id = 1},
        .quantity = qtt_quantity_finite(1),
        .observed = qtt_quantity_finite(1),
        .mode = QTT_OWNERSHIP_CONSUMED,
        .type = &string,
        .type_id = qtt_type_intern(types, &string, &type_error),
        .representation = QTT_REP_OWNED_HEAP,
    };
    QttFunctionSignature consume_signature = {
        .parameters = &parameter,
        .parameter_count = 1,
        .result_type = &integer,
        .result = {
            .mode = QTT_RESULT_IMMEDIATE,
            .type = &integer,
            .type_id = qtt_type_intern(types, &integer, &type_error),
            .representation = QTT_REP_IMMEDIATE,
        },
    };
    consume_signature.contract_fingerprint =
        qtt_signature_contract_fingerprint(&consume_signature);
    assert(qtt_signature_env_register(
        env, 1, "consumeString", &consume_signature, NULL) ==
        QTT_SIGNATURE_ENV_OK);

    evidence = qtt_closure_policy_infer_in_env(
        &consuming_invoke, &consuming_lambda, env, 1);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_UNIQUE);
    assert(evidence.reason == QTT_CLOSURE_POLICY_SINGLE_DIRECT_CALL);
    assert(evidence.moved_out_capture_count == 1);
    assert(evidence.unsupported_exit_capture_count == 0);
    assert(qtt_closure_capture_exit_in_env(
        &consuming_lambda, heap_capture, env, 1) ==
        QTT_CLOSURE_FIELD_MOVE_OUT);
    assert(qtt_closure_policy_verify_in_env(
        &consuming_invoke, &consuming_lambda, env, 1, evidence));

    parameter.mode = QTT_OWNERSHIP_BORROWED;
    consume_signature.contract_fingerprint =
        qtt_signature_contract_fingerprint(&consume_signature);
    evidence = qtt_closure_policy_infer_in_env(
        &consuming_invoke, &consuming_lambda, env, 1);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_UNIQUE);
    assert(evidence.released_capture_count == 1);
    assert(evidence.moved_out_capture_count == 0);
    assert(qtt_closure_capture_exit_in_env(
        &consuming_lambda, heap_capture, env, 1) ==
        QTT_CLOSURE_FIELD_RELEASE);

    QttCoreVar alias = {.module_id = 1, .binder_id = 71};
    QttCoreNode alias_use = {
        .kind = QTT_CORE_VAR, .type = &string, .var = alias};
    QttCoreNode nested_argument = {
        .kind = QTT_CORE_LET, .type = &string,
        .let = {.binding = alias, .value = &heap_use,
                .body = &alias_use}};
    consume_arguments[0] = &nested_argument;
    evidence = qtt_closure_policy_infer_in_env(
        &consuming_invoke, &consuming_lambda, env, 1);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_SHARED);
    assert(evidence.unsupported_exit_capture_count == 1);
    consume_arguments[0] = &heap_use;

    consume_signature.contract_fingerprint ^= 1;
    evidence = qtt_closure_policy_infer_in_env(
        &consuming_invoke, &consuming_lambda, env, 1);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_SHARED);
    assert(evidence.unsupported_exit_capture_count == 1);
    consume_signature.contract_fingerprint ^= 1;

    evidence =
        qtt_closure_policy_infer(&consuming_invoke, &consuming_lambda);
    assert(evidence.storage == QTT_CLOSURE_STORAGE_SHARED);
    assert(evidence.unsupported_exit_capture_count == 1);
    qtt_signature_env_free(env);
    qtt_type_arena_free(types);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_closure_policy_test.c"
            executable = directory / "qtt_closure_policy_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(ROOT / "src"), str(harness),
                    str(ROOT / "src" / "qtt" / "core.c"),
                    str(ROOT / "src" / "qtt" / "closure_policy.c"),
                    str(ROOT / "src" / "qtt" / "call.c"),
                    str(ROOT / "src" / "qtt" / "signature.c"),
                    str(ROOT / "src" / "effects" / "effect.c"),
                    str(ROOT / "src" / "qtt" / "place.c"),
                    str(ROOT / "src" / "qtt" / "signature_env.c"),
                    str(ROOT / "src" / "qtt" / "type_identity.c"),
                    str(ROOT / "src" / "qtt" / "quantity.c"),
                    str(ROOT / "src" / "qtt" / "graded.c"),
                    str(ROOT / "src" / "qtt" / "demand.c"),
                    str(ROOT / "src" / "qtt" / "resource.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
