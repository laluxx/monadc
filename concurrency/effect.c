#include "effect.h"

#include <stdbool.h>

static const char *operation_name(ConcurrencyEffectOperation operation) {
    switch (operation) {
    case CONCURRENCY_EFFECT_SPAWN: return "spawn";
    case CONCURRENCY_EFFECT_CANCEL: return "cancel";
    case CONCURRENCY_EFFECT_CHECKPOINT: return "checkpoint";
    case CONCURRENCY_EFFECT_FAIL: return "fail";
    case CONCURRENCY_EFFECT_CLEANUP_FAIL: return "cleanup-fail";
    case CONCURRENCY_EFFECT_CLEANUP: return "cleanup-complete";
    case CONCURRENCY_EFFECT_JOIN: return "join";
    }
    return NULL;
}

QttEffectAtom concurrency_effect_atom(
    ConcurrencyEffectOperation operation,
    uint64_t scope_capability_id, uint64_t task_type_id) {
    const char *name = operation_name(operation);
    return (QttEffectAtom){
        .kind = QTT_EFFECT_ASYNC,
        .constructor_id = (uint64_t)operation + 1,
        .type_id = task_type_id,
        .capability_id = scope_capability_id,
        .name = "Concurrency",
        .operation = name,
        .resumption = qtt_quantity_finite(1),
        .scoped = true,
    };
}

static bool operation_for_step(
    ConcurrencyStepKind kind, ConcurrencyEffectOperation *operation) {
    switch (kind) {
    case CONCURRENCY_STEP_SPAWN:
        *operation = CONCURRENCY_EFFECT_SPAWN;
        return true;
    case CONCURRENCY_STEP_CANCEL:
        *operation = CONCURRENCY_EFFECT_CANCEL;
        return true;
    case CONCURRENCY_STEP_CHECKPOINT:
        *operation = CONCURRENCY_EFFECT_CHECKPOINT;
        return true;
    case CONCURRENCY_STEP_FAIL:
        *operation = CONCURRENCY_EFFECT_FAIL;
        return true;
    case CONCURRENCY_STEP_CLEANUP_FAIL:
        *operation = CONCURRENCY_EFFECT_CLEANUP_FAIL;
        return true;
    case CONCURRENCY_STEP_CLEANUP_COMPLETE:
        *operation = CONCURRENCY_EFFECT_CLEANUP;
        return true;
    case CONCURRENCY_STEP_JOIN:
        *operation = CONCURRENCY_EFFECT_JOIN;
        return true;
    }
    return false;
}

ConcurrencyEffectVerification concurrency_verify_trace_effects(
    const ConcurrencyTrace *trace, const QttEffectSolver *solver,
    const QttEffectRow *effects,
    uint64_t scope_capability_id, uint64_t task_type_id) {
    if (!trace || !solver || !effects ||
        !scope_capability_id || !task_type_id)
        return CONCURRENCY_EFFECT_INVALID_ARGUMENT;
    if (concurrency_verify_trace(trace).error != CONCURRENCY_VALID)
        return CONCURRENCY_EFFECT_INVALID_TRACE;

    bool required[CONCURRENCY_EFFECT_JOIN + 1] = {false};
    for (size_t i = 0; i < trace->step_count; i++) {
        ConcurrencyEffectOperation operation;
        if (!operation_for_step(trace->steps[i].kind, &operation))
            return CONCURRENCY_EFFECT_INVALID_TRACE;
        required[operation] = true;
    }
    for (int operation = CONCURRENCY_EFFECT_SPAWN;
         operation <= CONCURRENCY_EFFECT_JOIN; operation++) {
        if (!required[operation]) continue;
        QttEffectAtom atom = concurrency_effect_atom(
            (ConcurrencyEffectOperation)operation,
            scope_capability_id, task_type_id);
        if (qtt_effect_atom_count(solver, effects, &atom) == 0)
            return CONCURRENCY_EFFECT_MISSING;
    }
    return CONCURRENCY_EFFECTS_VALID;
}
