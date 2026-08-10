#include "closure.h"

#include <stdlib.h>

static uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static uint64_t capture_fingerprint(const QttClosureCapture *capture) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, capture->source.module_id);
    hash = mix(hash, capture->source.binder_id);
    hash = mix(hash, capture->slot.module_id);
    hash = mix(hash, capture->slot.binder_id);
    hash = mix(hash, capture->field_id.storage_module);
    hash = mix(hash, capture->field_id.closure_id);
    hash = mix(hash, capture->field_id.instance_id);
    hash = mix(hash, capture->field_id.ordinal);
    hash = mix(hash, capture->type_fingerprint);
    hash = mix(hash, (uint64_t)capture->representation);
    hash = mix(hash, (uint64_t)capture->transfer);
    return mix(hash, (uint64_t)capture->exit);
}

bool qtt_closure_field_id_equal(
    QttClosureFieldId left, QttClosureFieldId right) {
    return left.storage_module == right.storage_module &&
        left.closure_id == right.closure_id &&
        left.instance_id == right.instance_id &&
        left.ordinal == right.ordinal;
}

static bool attach_capture_destructor(QttClosureCapture *capture,
                                      QttClosureError *error) {
    if (capture->representation != QTT_REP_OWNED_HEAP &&
        capture->representation != QTT_REP_INLINE)
        return true;
    QttDropPlanError drop_error = QTT_DROP_PLAN_OK;
    capture->destructor =
        qtt_destructor_descriptor_build(capture->type, &drop_error);
    if (!capture->destructor) {
        if (error) *error = drop_error == QTT_DROP_PLAN_OUT_OF_MEMORY
            ? QTT_CLOSURE_OUT_OF_MEMORY : QTT_CLOSURE_DESTRUCTOR_ERROR;
        return false;
    }
    if (capture->representation == QTT_REP_INLINE &&
        capture->destructor->plan->kind != QTT_DROP_NOOP) {
        if (error) *error = QTT_CLOSURE_STRUCTURAL_CAPTURE_UNSUPPORTED;
        return false;
    }
    if (capture->representation == QTT_REP_INLINE) {
        qtt_destructor_descriptor_free(capture->destructor);
        capture->destructor = NULL;
    }
    return true;
}

uint64_t qtt_closure_plan_fingerprint(const QttClosurePlan *plan) {
    if (!plan) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, plan->environment_module);
    hash = mix(hash, plan->first_slot);
    hash = mix(hash, plan->source_module);
    hash = mix(hash, plan->closure_id);
    hash = mix(hash, plan->instance_id);
    hash = mix(hash, (uint64_t)plan->storage);
    hash = mix(hash, plan->capture_count);
    for (size_t i = 0; i < plan->capture_count; i++)
        hash = mix(hash, plan->captures[i].field_fingerprint);
    return hash;
}

static bool plan_certificate_valid(const QttClosurePlan *plan) {
    if (!plan ||
        (plan->storage != QTT_CLOSURE_STORAGE_UNIQUE &&
         plan->storage != QTT_CLOSURE_STORAGE_SHARED) ||
        plan->contract_fingerprint != qtt_closure_plan_fingerprint(plan))
        return false;
    for (size_t i = 0; i < plan->capture_count; i++) {
        const QttClosureCapture *capture = &plan->captures[i];
        if (!capture->type ||
            capture->type_fingerprint !=
                qtt_type_fingerprint(capture->type) ||
            capture->representation !=
                qtt_resource_classify_type(capture->type) ||
            capture->transfer !=
                (capture->representation == QTT_REP_OWNED_HEAP
                    ? (plan->storage == QTT_CLOSURE_STORAGE_UNIQUE
                        ? QTT_CLOSURE_CAPTURE_MOVE
                        : QTT_CLOSURE_CAPTURE_RETAIN)
                    : QTT_CLOSURE_CAPTURE_VALUE) ||
            (capture->representation == QTT_REP_OWNED_HEAP) !=
                (capture->destructor != NULL) ||
            (capture->exit != QTT_CLOSURE_FIELD_RELEASE &&
             capture->exit != QTT_CLOSURE_FIELD_MOVE_OUT) ||
            (plan->storage == QTT_CLOSURE_STORAGE_SHARED &&
             capture->exit != QTT_CLOSURE_FIELD_RELEASE) ||
            !qtt_closure_field_id_equal(
                capture->field_id, (QttClosureFieldId){
                    .storage_module = plan->environment_module,
                    .closure_id = plan->closure_id,
                    .instance_id = plan->instance_id,
                    .ordinal = i,
                }) ||
            capture->field_fingerprint != capture_fingerprint(capture) ||
            (capture->destructor &&
             !qtt_destructor_descriptor_verify(
                 capture->destructor, capture->type)))
            return false;
    }
    return true;
}

QttClosurePlan *qtt_closure_plan_build(const QttCoreNode *lambda,
                                       uint64_t environment_module,
                                       uint64_t first_slot,
                                       QttClosureError *error) {
    return qtt_closure_plan_build_with_policy(
        lambda, environment_module, first_slot,
        QTT_CLOSURE_STORAGE_SHARED, error);
}

static QttClosurePlan *closure_plan_build(
    const QttCoreNode *lambda, uint64_t environment_module,
    uint64_t first_slot, QttClosureStorage storage,
    const QttSignatureEnv *signatures, uint64_t module_id,
    QttClosureError *error);
static bool closure_plan_verify(
    const QttClosurePlan *plan, const QttCoreNode *lambda,
    const QttSignatureEnv *signatures, uint64_t module_id,
    QttClosureError *error);

QttClosurePlan *qtt_closure_plan_infer(
    const QttCoreNode *root, const QttCoreNode *lambda,
    uint64_t environment_module, uint64_t first_slot,
    QttClosurePolicyEvidence *policy, QttClosureError *error) {
    QttClosurePolicyEvidence inferred =
        qtt_closure_policy_infer(root, lambda);
    if (policy) *policy = inferred;
    return qtt_closure_plan_build_with_policy(
        lambda, environment_module, first_slot,
        inferred.storage == QTT_CLOSURE_STORAGE_UNIQUE
            ? QTT_CLOSURE_STORAGE_UNIQUE
            : QTT_CLOSURE_STORAGE_SHARED,
        error);
}

QttClosurePlan *qtt_closure_plan_infer_in_env(
    const QttCoreNode *root, const QttCoreNode *lambda,
    const QttSignatureEnv *signatures, uint64_t module_id,
    uint64_t environment_module, uint64_t first_slot,
    QttClosurePolicyEvidence *policy, QttClosureError *error) {
    if (!signatures || !module_id) {
        if (error) *error = QTT_CLOSURE_INVALID_PLAN;
        return NULL;
    }
    QttClosurePolicyEvidence inferred =
        qtt_closure_policy_infer_in_env(
            root, lambda, signatures, module_id);
    if (policy) *policy = inferred;
    return closure_plan_build(
        lambda, environment_module, first_slot,
        inferred.storage == QTT_CLOSURE_STORAGE_UNIQUE
            ? QTT_CLOSURE_STORAGE_UNIQUE
            : QTT_CLOSURE_STORAGE_SHARED,
        signatures, module_id, error);
}

QttClosurePlan *qtt_closure_plan_build_with_policy(
    const QttCoreNode *lambda, uint64_t environment_module,
    uint64_t first_slot, QttClosureStorage storage,
    QttClosureError *error) {
    return closure_plan_build(
        lambda, environment_module, first_slot,
        storage, NULL, 0, error);
}

static QttClosurePlan *closure_plan_build(
    const QttCoreNode *lambda, uint64_t environment_module,
    uint64_t first_slot, QttClosureStorage storage,
    const QttSignatureEnv *signatures, uint64_t module_id,
    QttClosureError *error) {
    if (!lambda || lambda->kind != QTT_CORE_LAMBDA ||
        (storage != QTT_CLOSURE_STORAGE_UNIQUE &&
         storage != QTT_CLOSURE_STORAGE_SHARED)) {
        if (error) *error = QTT_CLOSURE_NOT_A_LAMBDA;
        return NULL;
    }
    QttCoreVar *free_vars = NULL;
    size_t free_count = 0;
    QttCoreError core_error =
        qtt_core_lambda_captures(lambda, &free_vars, &free_count);
    if (core_error != QTT_CORE_OK) {
        if (error) *error = core_error == QTT_CORE_OUT_OF_MEMORY
            ? QTT_CLOSURE_OUT_OF_MEMORY : QTT_CLOSURE_NOT_A_LAMBDA;
        return NULL;
    }
    if (free_count && first_slot > UINT64_MAX - (free_count - 1)) {
        free(free_vars);
        if (error) *error = QTT_CLOSURE_SLOT_OVERFLOW;
        return NULL;
    }
    QttClosurePlan *plan = calloc(1, sizeof(*plan));
    if (!plan) {
        free(free_vars);
        if (error) *error = QTT_CLOSURE_OUT_OF_MEMORY;
        return NULL;
    }
    plan->capture_count = free_count;
    plan->environment_module = environment_module;
    plan->first_slot = first_slot;
    plan->storage = storage;
    if (free_count)
        plan->captures = calloc(free_count, sizeof(*plan->captures));
    if (free_count && !plan->captures) {
        free(free_vars);
        free(plan);
        if (error) *error = QTT_CLOSURE_OUT_OF_MEMORY;
        return NULL;
    }
    for (size_t i = 0; i < free_count; i++) {
        plan->captures[i].source = free_vars[i];
        plan->captures[i].slot = (QttCoreVar){
            .module_id = environment_module,
            .binder_id = first_slot + i,
        };
        plan->captures[i].field_id = (QttClosureFieldId){
            .storage_module = environment_module,
            .ordinal = i,
        };
        plan->captures[i].type =
            qtt_core_var_type(lambda->lambda.body, free_vars[i]);
        if (!plan->captures[i].type) {
            if (error) *error = QTT_CLOSURE_MISSING_TYPE;
            free(free_vars);
            qtt_closure_plan_free(plan);
            return NULL;
        }
        plan->captures[i].type_fingerprint =
            qtt_type_fingerprint(plan->captures[i].type);
        plan->captures[i].representation =
            qtt_resource_classify_type(plan->captures[i].type);
        if (plan->captures[i].representation == QTT_REP_UNKNOWN) {
            if (error) *error = QTT_CLOSURE_UNKNOWN_REPRESENTATION;
            free(free_vars);
            qtt_closure_plan_free(plan);
            return NULL;
        }
        plan->captures[i].transfer =
            plan->captures[i].representation == QTT_REP_OWNED_HEAP
                ? (storage == QTT_CLOSURE_STORAGE_UNIQUE
                    ? QTT_CLOSURE_CAPTURE_MOVE
                    : QTT_CLOSURE_CAPTURE_RETAIN)
                : QTT_CLOSURE_CAPTURE_VALUE;
        plan->captures[i].exit =
            storage == QTT_CLOSURE_STORAGE_UNIQUE
                ? signatures
                    ? qtt_closure_capture_exit_in_env(
                        lambda, free_vars[i], signatures, module_id)
                    : qtt_closure_capture_exit(lambda, free_vars[i])
                : QTT_CLOSURE_FIELD_RELEASE;
        if (plan->captures[i].exit ==
            QTT_CLOSURE_FIELD_EXIT_UNSUPPORTED) {
            if (error) *error = QTT_CLOSURE_INVALID_PLAN;
            free(free_vars);
            qtt_closure_plan_free(plan);
            return NULL;
        }
        if (!attach_capture_destructor(&plan->captures[i], error)) {
            free(free_vars);
            qtt_closure_plan_free(plan);
            return NULL;
        }
        plan->captures[i].field_fingerprint =
            capture_fingerprint(&plan->captures[i]);
    }
    free(free_vars);
    plan->contract_fingerprint = qtt_closure_plan_fingerprint(plan);
    if (error) *error = QTT_CLOSURE_OK;
    return plan;
}

bool qtt_closure_plan_bind_environment(
    QttClosurePlan *plan, const QttCoreNode *lambda,
    const QttClosureEnvironment *environment,
    const QttCoreVar *parameters, size_t parameter_count,
    QttClosureError *error) {
    return qtt_closure_plan_bind_environment_in_env(
        plan, lambda, environment, parameters, parameter_count,
        NULL, 0, error);
}

bool qtt_closure_plan_bind_environment_in_env(
    QttClosurePlan *plan, const QttCoreNode *lambda,
    const QttClosureEnvironment *environment,
    const QttCoreVar *parameters, size_t parameter_count,
    const QttSignatureEnv *signatures, uint64_t module_id,
    QttClosureError *error) {
    bool plan_valid = signatures
        ? module_id && closure_plan_verify(
            plan, lambda, signatures, module_id, error)
        : closure_plan_verify(
            plan, lambda, NULL, 0, error);
    if (!plan_valid ||
        !qtt_environment_validate(environment) ||
        environment->slot_count != plan->capture_count ||
        environment->parameter_count != parameter_count ||
        (parameter_count && !parameters)) {
        if (error && *error == QTT_CLOSURE_OK)
            *error = QTT_CLOSURE_ENVIRONMENT_MISMATCH;
        return false;
    }
    for (size_t i = 0; i < environment->slot_count; i++) {
        QttEnvironmentOrigin origin = environment->origins[i];
        QttCoreVar expected = {0};
        if (origin.kind == QTT_ENVIRONMENT_PARAMETER) {
            if (origin.identity >= parameter_count) {
                if (error) *error = QTT_CLOSURE_ENVIRONMENT_MISMATCH;
                return false;
            }
            expected = parameters[origin.identity];
        } else if (origin.kind == QTT_ENVIRONMENT_LOCAL) {
            expected = (QttCoreVar){
                .module_id = environment->module_id,
                .binder_id = origin.identity,
            };
        } else {
            if (error)
                *error = QTT_CLOSURE_EXPRESSION_ORIGIN_UNSUPPORTED;
            return false;
        }
        if (!qtt_core_var_equal(plan->captures[i].source, expected)) {
            if (error) *error = QTT_CLOSURE_ENVIRONMENT_MISMATCH;
            return false;
        }
    }
    plan->source_module = environment->module_id;
    plan->closure_id = environment->closure_id;
    plan->instance_id = environment->instance_id;
    for (size_t i = 0; i < plan->capture_count; i++) {
        plan->captures[i].field_id.closure_id = environment->closure_id;
        plan->captures[i].field_id.instance_id = environment->instance_id;
        plan->captures[i].field_fingerprint =
            capture_fingerprint(&plan->captures[i]);
    }
    plan->contract_fingerprint = qtt_closure_plan_fingerprint(plan);
    if (error) *error = QTT_CLOSURE_OK;
    return true;
}

static bool closure_plan_verify(
    const QttClosurePlan *plan, const QttCoreNode *lambda,
    const QttSignatureEnv *signatures, uint64_t module_id,
    QttClosureError *error) {
    if (!plan || !lambda || lambda->kind != QTT_CORE_LAMBDA) {
        if (error) *error = QTT_CLOSURE_NOT_A_LAMBDA;
        return false;
    }
    QttCoreVar *free_vars = NULL;
    size_t free_count = 0;
    QttCoreError core_error =
        qtt_core_lambda_captures(lambda, &free_vars, &free_count);
    if (core_error != QTT_CORE_OK) {
        if (error) *error = core_error == QTT_CORE_OUT_OF_MEMORY
            ? QTT_CLOSURE_OUT_OF_MEMORY : QTT_CLOSURE_INVALID_PLAN;
        return false;
    }
    bool valid = free_count == plan->capture_count;
    for (size_t i = 0; valid && i < free_count; i++) {
        const QttClosureCapture *capture = &plan->captures[i];
        const Type *type =
            qtt_core_var_type(lambda->lambda.body, free_vars[i]);
        QttRepresentation representation =
            qtt_resource_classify_type(type);
        valid = type &&
            qtt_core_var_equal(capture->source, free_vars[i]) &&
            capture->slot.module_id == plan->environment_module &&
            capture->slot.binder_id == plan->first_slot + i &&
            qtt_closure_field_id_equal(
                capture->field_id, (QttClosureFieldId){
                    .storage_module = plan->environment_module,
                    .closure_id = plan->closure_id,
                    .instance_id = plan->instance_id,
                    .ordinal = i,
                }) &&
            capture->type == type &&
            capture->type_fingerprint == qtt_type_fingerprint(type) &&
            capture->representation == representation &&
            capture->transfer ==
                (representation == QTT_REP_OWNED_HEAP
                    ? (plan->storage == QTT_CLOSURE_STORAGE_UNIQUE
                        ? QTT_CLOSURE_CAPTURE_MOVE
                        : QTT_CLOSURE_CAPTURE_RETAIN)
                    : QTT_CLOSURE_CAPTURE_VALUE) &&
            capture->exit ==
                (plan->storage == QTT_CLOSURE_STORAGE_UNIQUE
                    ? signatures
                        ? qtt_closure_capture_exit_in_env(
                            lambda, free_vars[i],
                            signatures, module_id)
                        : qtt_closure_capture_exit(
                            lambda, free_vars[i])
                    : QTT_CLOSURE_FIELD_RELEASE) &&
            capture->field_fingerprint == capture_fingerprint(capture) &&
            (representation == QTT_REP_OWNED_HEAP
                ? qtt_destructor_descriptor_verify(
                    capture->destructor, type)
                : capture->destructor == NULL);
    }
    free(free_vars);
    valid = valid &&
        plan->contract_fingerprint == qtt_closure_plan_fingerprint(plan);
    if (error) *error =
        valid ? QTT_CLOSURE_OK : QTT_CLOSURE_INVALID_PLAN;
    return valid;
}

bool qtt_closure_plan_verify(const QttClosurePlan *plan,
                             const QttCoreNode *lambda,
                             QttClosureError *error) {
    return closure_plan_verify(
        plan, lambda, NULL, 0, error);
}

bool qtt_closure_plan_verify_in_env(
    const QttClosurePlan *plan, const QttCoreNode *lambda,
    const QttSignatureEnv *signatures, uint64_t module_id,
    QttClosureError *error) {
    if (!signatures || !module_id) {
        if (error) *error = QTT_CLOSURE_INVALID_PLAN;
        return false;
    }
    return closure_plan_verify(
        plan, lambda, signatures, module_id, error);
}

QttResourceBlock *qtt_closure_emit_construction(
    const QttClosurePlan *plan, QttClosureError *error) {
    if (!plan_certificate_valid(plan)) {
        if (error) *error = QTT_CLOSURE_INVALID_PLAN;
        return NULL;
    }
    QttResourceBlock *block = calloc(1, sizeof(*block));
    if (!block) {
        if (error) *error = QTT_CLOSURE_OUT_OF_MEMORY;
        return NULL;
    }
    for (size_t i = 0; i < plan->capture_count; i++)
        if (plan->captures[i].transfer != QTT_CLOSURE_CAPTURE_VALUE)
            block->count++;
    if (block->count)
        block->ops = calloc(block->count, sizeof(*block->ops));
    if (block->count && !block->ops) {
        free(block);
        if (error) *error = QTT_CLOSURE_OUT_OF_MEMORY;
        return NULL;
    }
    size_t emitted = 0;
    for (size_t i = 0; i < plan->capture_count; i++) {
        const QttClosureCapture *capture = &plan->captures[i];
        if (capture->transfer == QTT_CLOSURE_CAPTURE_RETAIN)
            block->ops[emitted++] =
                qtt_resource_dup(capture->source, capture->slot);
        else if (capture->transfer == QTT_CLOSURE_CAPTURE_MOVE)
            block->ops[emitted++] =
                qtt_resource_rehome(capture->source, capture->slot);
    }
    if (error) *error = QTT_CLOSURE_OK;
    return block;
}

QttResourceBlock *qtt_closure_emit_retains(const QttClosurePlan *plan,
                                           QttClosureError *error) {
    return qtt_closure_emit_construction(plan, error);
}

QttResourceBlock *qtt_closure_emit_finalization(
    const QttClosurePlan *plan, QttClosureError *error) {
    if (!plan_certificate_valid(plan)) {
        if (error) *error = QTT_CLOSURE_INVALID_PLAN;
        return NULL;
    }
    QttResourceBlock *block = calloc(1, sizeof(*block));
    if (!block) {
        if (error) *error = QTT_CLOSURE_OUT_OF_MEMORY;
        return NULL;
    }
    for (size_t i = 0; i < plan->capture_count; i++)
        if (plan->captures[i].destructor)
            block->count++;
    if (block->count)
        block->ops = calloc(block->count, sizeof(*block->ops));
    if (block->count && !block->ops) {
        free(block);
        if (error) *error = QTT_CLOSURE_OUT_OF_MEMORY;
        return NULL;
    }
    size_t emitted = 0;
    for (size_t i = 0; i < plan->capture_count; i++)
        if (plan->captures[i].destructor)
            block->ops[emitted++] =
                plan->captures[i].exit == QTT_CLOSURE_FIELD_MOVE_OUT
                    ? qtt_resource_move(plan->captures[i].slot)
                    : qtt_resource_drop(plan->captures[i].slot);
    if (error) *error = QTT_CLOSURE_OK;
    return block;
}

QttResourceBlock *qtt_closure_emit_releases(const QttClosurePlan *plan,
                                            QttClosureError *error) {
    return qtt_closure_emit_finalization(plan, error);
}

void qtt_closure_plan_free(QttClosurePlan *plan) {
    if (!plan) return;
    for (size_t i = 0; i < plan->capture_count; i++)
        qtt_destructor_descriptor_free(plan->captures[i].destructor);
    free(plan->captures);
    free(plan);
}
