"""Ownership plans for closure environments."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


class QttClosureTests(unittest.TestCase):
    def test_typed_closure_contract_drives_retains_and_destruction(self):
        source = r'''
#include "qtt/closure.h"
#include <assert.h>

int main(void) {
    QttCoreVar x = {.module_id = 1, .binder_id = 7};
    QttCoreVar n = {.module_id = 1, .binder_id = 8};
    Type string = {.kind = TYPE_STRING};
    Type integer = {.kind = TYPE_INT};
    QttCoreNode use_x = {
        .kind = QTT_CORE_VAR, .type = &string, .var = x};
    QttCoreNode use_n = {
        .kind = QTT_CORE_VAR, .type = &integer, .var = n};
    QttCoreNode *uses[] = {&use_x, &use_n, &use_x};
    QttCoreNode body = {
        .kind = QTT_CORE_SEQUENCE, .type = &string,
        .sequence = {.items = uses, .count = 3},
    };
    QttCoreNode lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &string,
        .lambda = {.body = &body},
    };

    QttClosureError error = QTT_CLOSURE_OK;
    QttClosurePlan *plan =
        qtt_closure_plan_build(&lambda, 99, 1000, &error);
    assert(plan && error == QTT_CLOSURE_OK);
    assert(plan->capture_count == 2);
    assert(qtt_closure_plan_verify(plan, &lambda, &error));
    assert(qtt_core_var_equal(plan->captures[0].source, x));
    assert(plan->captures[0].slot.module_id == 99);
    assert(plan->captures[0].slot.binder_id == 1000);
    assert(plan->captures[0].field_id.storage_module == 99);
    assert(plan->captures[0].field_id.closure_id == 0);
    assert(plan->captures[0].field_id.instance_id == 0);
    assert(plan->captures[0].field_id.ordinal == 0);
    assert(plan->captures[0].type == &string);
    assert(plan->captures[0].representation == QTT_REP_OWNED_HEAP);
    assert(plan->captures[0].transfer == QTT_CLOSURE_CAPTURE_RETAIN);
    assert(plan->captures[0].destructor);
    assert(qtt_destructor_descriptor_verify(
        plan->captures[0].destructor, &string));
    assert(qtt_core_var_equal(plan->captures[1].source, n));
    assert(plan->captures[1].slot.binder_id == 1001);
    assert(plan->captures[1].representation == QTT_REP_IMMEDIATE);
    assert(plan->captures[1].transfer == QTT_CLOSURE_CAPTURE_VALUE);
    assert(!plan->captures[1].destructor);
    assert(plan->contract_fingerprint ==
           qtt_closure_plan_fingerprint(plan));

    QttEnvironmentOrigin origins[] = {
        qtt_environment_local(7), qtt_environment_local(8)};
    QttClosureEnvironment *environment = qtt_environment_new(
        501, 1, 77, origins, 2, 0);
    assert(environment);
    assert(qtt_closure_plan_bind_environment(
        plan, &lambda, environment, NULL, 0, &error));
    assert(plan->source_module == 1);
    assert(plan->closure_id == 77);
    assert(plan->instance_id == 501);
    assert(plan->captures[0].field_id.closure_id == 77);
    assert(plan->captures[0].field_id.instance_id == 501);
    assert(plan->captures[1].field_id.ordinal == 1);
    assert(qtt_closure_plan_verify(plan, &lambda, &error));

    QttEnvironmentOrigin wrong_origins[] = {
        qtt_environment_local(8), qtt_environment_local(7)};
    QttClosureEnvironment *wrong_environment = qtt_environment_new(
        502, 1, 77, wrong_origins, 2, 0);
    assert(wrong_environment);
    assert(!qtt_closure_plan_bind_environment(
        plan, &lambda, wrong_environment, NULL, 0, &error));
    assert(error == QTT_CLOSURE_ENVIRONMENT_MISMATCH);

    LayoutField owning_field = {.name = "text", .type = &string};
    Type inline_owner = {
        .kind = TYPE_LAYOUT, .layout_name = "InlineOwner",
        .layout_fields = &owning_field, .layout_field_count = 1,
        .layout_is_inline = true,
    };
    QttCoreVar aggregate = {.module_id = 1, .binder_id = 9};
    QttCoreNode use_aggregate = {
        .kind = QTT_CORE_VAR, .type = &inline_owner, .var = aggregate};
    QttCoreNode aggregate_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &inline_owner,
        .lambda = {.body = &use_aggregate},
    };
    assert(!qtt_closure_plan_build(
        &aggregate_lambda, 99, 2000, &error));
    assert(error == QTT_CLOSURE_STRUCTURAL_CAPTURE_UNSUPPORTED);

    QttClosurePlan *unique = qtt_closure_plan_build_with_policy(
        &lambda, 100, 3000, QTT_CLOSURE_STORAGE_UNIQUE, &error);
    assert(unique && error == QTT_CLOSURE_OK);
    assert(unique->captures[0].transfer == QTT_CLOSURE_CAPTURE_MOVE);
    assert(unique->captures[0].exit ==
           QTT_CLOSURE_FIELD_MOVE_OUT);
    assert(unique->captures[1].transfer == QTT_CLOSURE_CAPTURE_VALUE);
    QttResourceBlock *unique_ops =
        qtt_closure_emit_construction(unique, &error);
    assert(unique_ops && unique_ops->count == 1);
    assert(unique_ops->ops[0].kind == QTT_RESOURCE_REHOME);
    assert(qtt_core_var_equal(unique_ops->ops[0].var, x));
    assert(qtt_core_var_equal(
        unique_ops->ops[0].target, unique->captures[0].slot));
    QttResourceOp unique_complete_ops[] = {
        qtt_resource_rehome(x, unique->captures[0].slot),
        qtt_resource_move(unique->captures[0].slot),
    };
    QttResourceBlock unique_complete = {unique_complete_ops, 2};
    assert(qtt_resource_verify(&unique_complete, &x, 1).error ==
           QTT_RESOURCE_VALID);
    QttResourceBlock *unique_finalization =
        qtt_closure_emit_finalization(unique, &error);
    assert(unique_finalization && unique_finalization->count == 1);
    assert(unique_finalization->ops[0].kind == QTT_RESOURCE_MOVE);
    assert(qtt_core_var_equal(
        unique_finalization->ops[0].var, unique->captures[0].slot));
    qtt_resource_block_free(unique_finalization);
    qtt_resource_block_free(unique_ops);
    qtt_closure_plan_free(unique);

    QttCoreNode direct_call = {
        .kind = QTT_CORE_APPLY, .type = &string,
        .apply = {.callee = &lambda},
    };
    QttClosurePolicyEvidence inferred_policy = {0};
    unique = qtt_closure_plan_infer(
        &direct_call, &lambda, 100, 4000, &inferred_policy, &error);
    assert(unique && unique->storage == QTT_CLOSURE_STORAGE_UNIQUE);
    assert(inferred_policy.reason ==
           QTT_CLOSURE_POLICY_SINGLE_DIRECT_CALL);
    qtt_closure_plan_free(unique);

    QttCoreVar consumed = {.module_id = 1, .binder_id = 70};
    QttCoreNode consumed_use = {
        .kind = QTT_CORE_VAR, .type = &string, .var = consumed};
    QttCoreNode consume_global = {
        .kind = QTT_CORE_GLOBAL, .type = &integer,
        .global = {.name = "consumeString"}};
    QttCoreNode *consume_arguments[] = {&consumed_use};
    QttCoreNode consume_call = {
        .kind = QTT_CORE_APPLY, .type = &integer,
        .apply = {.callee = &consume_global,
                  .arguments = consume_arguments,
                  .argument_count = 1}};
    QttCoreNode consuming_lambda = {
        .kind = QTT_CORE_LAMBDA, .type = &integer,
        .lambda = {.body = &consume_call}};
    QttCoreNode consuming_invoke = {
        .kind = QTT_CORE_APPLY, .type = &integer,
        .apply = {.callee = &consuming_lambda}};
    QttTypeArena *types = qtt_type_arena_new();
    QttSignatureEnv *signatures = qtt_signature_env_new(types);
    assert(types && signatures);
    QttTypeIdentityError type_error = QTT_TYPE_IDENTITY_OK;
    QttParameterContract consume_parameter = {
        .var = {.module_id = 1, .binder_id = 71},
        .quantity = qtt_quantity_finite(1),
        .observed = qtt_quantity_finite(1),
        .mode = QTT_OWNERSHIP_CONSUMED,
        .type = &string,
        .type_id = qtt_type_intern(types, &string, &type_error),
        .representation = QTT_REP_OWNED_HEAP,
    };
    QttFunctionSignature consume_signature = {
        .parameters = &consume_parameter,
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
               signatures, 1, "consumeString",
               &consume_signature, NULL) == QTT_SIGNATURE_ENV_OK);
    unique = qtt_closure_plan_infer_in_env(
        &consuming_invoke, &consuming_lambda, signatures, 1,
        100, 5000, &inferred_policy, &error);
    assert(unique && error == QTT_CLOSURE_OK);
    assert(unique->storage == QTT_CLOSURE_STORAGE_UNIQUE);
    assert(unique->captures[0].exit ==
           QTT_CLOSURE_FIELD_MOVE_OUT);
    assert(qtt_closure_plan_verify_in_env(
        unique, &consuming_lambda, signatures, 1, &error));
    assert(!qtt_closure_plan_verify(
        unique, &consuming_lambda, &error));
    QttEnvironmentOrigin consume_origin =
        qtt_environment_local(consumed.binder_id);
    QttClosureEnvironment *consume_environment =
        qtt_environment_new(
            601, 1, 88, &consume_origin, 1, 0);
    assert(consume_environment);
    assert(qtt_closure_plan_bind_environment_in_env(
        unique, &consuming_lambda, consume_environment,
        NULL, 0, signatures, 1, &error));
    assert(unique->source_module == 1);
    assert(unique->closure_id == 88);
    assert(unique->instance_id == 601);
    assert(qtt_closure_plan_verify_in_env(
        unique, &consuming_lambda, signatures, 1, &error));
    consume_parameter.mode = QTT_OWNERSHIP_BORROWED;
    consume_signature.contract_fingerprint =
        qtt_signature_contract_fingerprint(&consume_signature);
    assert(!qtt_closure_plan_verify_in_env(
        unique, &consuming_lambda, signatures, 1, &error));
    qtt_environment_free(consume_environment);
    qtt_closure_plan_free(unique);
    unique = qtt_closure_plan_infer_in_env(
        &consuming_invoke, &consuming_lambda, signatures, 1,
        100, 5000, &inferred_policy, &error);
    assert(unique && unique->captures[0].exit ==
           QTT_CLOSURE_FIELD_RELEASE);
    assert(qtt_closure_plan_verify_in_env(
        unique, &consuming_lambda, signatures, 1, &error));
    qtt_closure_plan_free(unique);
    qtt_signature_env_free(signatures);
    qtt_type_arena_free(types);

    QttClosurePlan *escaping = qtt_closure_plan_infer(
        &lambda, &lambda, 100, 4000, &inferred_policy, &error);
    assert(escaping && escaping->storage == QTT_CLOSURE_STORAGE_SHARED);
    assert(inferred_policy.reason == QTT_CLOSURE_POLICY_ESCAPES);
    qtt_closure_plan_free(escaping);

    QttResourceBlock *capture_ops =
        qtt_closure_emit_retains(plan, &error);
    assert(capture_ops && capture_ops->count == 1);
    assert(capture_ops->ops[0].kind == QTT_RESOURCE_DUP);
    assert(qtt_core_var_equal(capture_ops->ops[0].var, x));
    assert(qtt_core_var_equal(
        capture_ops->ops[0].target, plan->captures[0].slot));

    QttResourceBlock *release_ops =
        qtt_closure_emit_releases(plan, &error);
    assert(release_ops && release_ops->count == 1);
    assert(release_ops->ops[0].kind == QTT_RESOURCE_DROP);
    assert(qtt_core_var_equal(
        release_ops->ops[0].var, plan->captures[0].slot));

    QttResourceOp complete_ops[] = {
        qtt_resource_dup(x, plan->captures[0].slot),
        qtt_resource_drop(x),
        qtt_resource_drop(plan->captures[0].slot),
    };
    QttResourceBlock complete = {complete_ops, 3};
    assert(qtt_resource_verify(&complete, &x, 1).error ==
           QTT_RESOURCE_VALID);

    uint64_t valid_field = plan->captures[0].field_fingerprint;
    plan->captures[0].field_fingerprint ^= 1;
    assert(!qtt_closure_plan_verify(plan, &lambda, &error));
    assert(error == QTT_CLOSURE_INVALID_PLAN);
    assert(!qtt_closure_emit_retains(plan, &error));
    assert(error == QTT_CLOSURE_INVALID_PLAN);
    plan->captures[0].field_fingerprint = valid_field;
    assert(qtt_closure_plan_verify(plan, &lambda, &error));

    plan->captures[0].field_id.instance_id ^= 1;
    assert(!qtt_closure_plan_verify(plan, &lambda, &error));
    plan->captures[0].field_id.instance_id ^= 1;
    assert(qtt_closure_plan_verify(plan, &lambda, &error));

    uint64_t valid_destructor =
        plan->captures[0].destructor->id.plan_fingerprint;
    plan->captures[0].destructor->id.plan_fingerprint ^= 1;
    assert(!qtt_closure_plan_verify(plan, &lambda, &error));
    plan->captures[0].destructor->id.plan_fingerprint = valid_destructor;
    assert(qtt_closure_plan_verify(plan, &lambda, &error));

    qtt_resource_block_free(capture_ops);
    qtt_resource_block_free(release_ops);
    qtt_environment_free(wrong_environment);
    qtt_environment_free(environment);
    qtt_closure_plan_free(plan);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_closure_test.c"
            executable = directory / "qtt_closure_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(ROOT / "src"), str(harness),
                    str(ROOT / "src" / "qtt" / "core.c"),
                    str(ROOT / "src" / "qtt" / "resource.c"),
                    str(ROOT / "src" / "qtt" / "type_identity.c"),
                    str(ROOT / "src" / "qtt" / "drop.c"),
                    str(ROOT / "src" / "qtt" / "environment.c"),
                    str(ROOT / "src" / "qtt" / "quantity.c"),
                    str(ROOT / "src" / "qtt" / "demand.c"),
                    str(ROOT / "src" / "qtt" / "graded.c"),
                    str(ROOT / "src" / "qtt" / "signature.c"),
                    str(ROOT / "src" / "effects" / "effect.c"),
                    str(ROOT / "src" / "qtt" / "place.c"),
                    str(ROOT / "src" / "qtt" / "signature_env.c"),
                    str(ROOT / "src" / "qtt" / "call.c"),
                    str(ROOT / "src" / "qtt" / "closure_policy.c"),
                    str(ROOT / "src" / "qtt" / "closure.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
