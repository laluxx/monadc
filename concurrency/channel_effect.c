#include "effect.h"

#include <stdbool.h>
#include <stdint.h>

static const char *channel_operation_name(
    ConcurrencyChannelEffectOperation operation) {
    switch (operation) {
    case CONCURRENCY_CHANNEL_EFFECT_SEND: return "send";
    case CONCURRENCY_CHANNEL_EFFECT_RECEIVE: return "receive";
    case CONCURRENCY_CHANNEL_EFFECT_CLOSE: return "close";
    case CONCURRENCY_CHANNEL_EFFECT_CANCEL: return "cancel";
    }
    return NULL;
}

QttEffectAtom concurrency_channel_effect_atom(
    ConcurrencyChannelEffectOperation operation,
    uint64_t scope_capability_id, uint64_t channel_type_id) {
    return (QttEffectAtom){
        .kind = QTT_EFFECT_ASYNC,
        .constructor_id = UINT64_C(0x100) + (uint64_t)operation,
        .type_id = channel_type_id,
        .capability_id = scope_capability_id,
        .name = "Concurrency.Channel",
        .operation = channel_operation_name(operation),
        .resumption = qtt_quantity_finite(1),
        .scoped = true,
    };
}

ConcurrencyChannelEffectVerification concurrency_authorize_channel_step(
    ConcurrencyChannelStepKind step, const QttEffectSolver *solver,
    const QttEffectRow *effects,
    uint64_t scope_capability_id, uint64_t channel_type_id) {
    if (!solver || !effects || !scope_capability_id || !channel_type_id)
        return CONCURRENCY_CHANNEL_EFFECT_INVALID_ARGUMENT;
    bool required[CONCURRENCY_CHANNEL_EFFECT_CANCEL + 1] = {false};
    switch (step) {
    case CONCURRENCY_CHANNEL_STEP_SEND:
        required[CONCURRENCY_CHANNEL_EFFECT_SEND] = true;
        break;
    case CONCURRENCY_CHANNEL_STEP_RECEIVE:
        required[CONCURRENCY_CHANNEL_EFFECT_RECEIVE] = true;
        break;
    case CONCURRENCY_CHANNEL_STEP_RENDEZVOUS:
        required[CONCURRENCY_CHANNEL_EFFECT_SEND] = true;
        required[CONCURRENCY_CHANNEL_EFFECT_RECEIVE] = true;
        break;
    case CONCURRENCY_CHANNEL_STEP_CLOSE:
        required[CONCURRENCY_CHANNEL_EFFECT_CLOSE] = true;
        break;
    case CONCURRENCY_CHANNEL_STEP_CANCEL:
        required[CONCURRENCY_CHANNEL_EFFECT_CANCEL] = true;
        break;
    default:
        return CONCURRENCY_CHANNEL_EFFECT_INVALID_TRACE;
    }
    for (int operation = CONCURRENCY_CHANNEL_EFFECT_SEND;
         operation <= CONCURRENCY_CHANNEL_EFFECT_CANCEL; operation++) {
        if (!required[operation]) continue;
        QttEffectAtom atom = concurrency_channel_effect_atom(
            (ConcurrencyChannelEffectOperation)operation,
            scope_capability_id, channel_type_id);
        if (!atom.operation ||
            qtt_effect_atom_count(solver, effects, &atom) == 0)
            return CONCURRENCY_CHANNEL_EFFECT_MISSING;
    }
    return CONCURRENCY_CHANNEL_EFFECTS_VALID;
}

ConcurrencyChannelEffectVerification concurrency_verify_channel_effects(
    const ConcurrencyChannelTrace *trace, const QttEffectSolver *solver,
    const QttEffectRow *effects,
    uint64_t scope_capability_id, uint64_t channel_type_id) {
    if (!trace || !solver || !effects ||
        !scope_capability_id || !channel_type_id)
        return CONCURRENCY_CHANNEL_EFFECT_INVALID_ARGUMENT;
    if (concurrency_channel_verify(trace).error != CONCURRENCY_CHANNEL_VALID)
        return CONCURRENCY_CHANNEL_EFFECT_INVALID_TRACE;

    for (size_t i = 0; i < trace->step_count; i++) {
        ConcurrencyChannelEffectVerification authorized =
            concurrency_authorize_channel_step(
                trace->steps[i].kind, solver, effects,
                scope_capability_id, channel_type_id);
        if (authorized != CONCURRENCY_CHANNEL_EFFECTS_VALID)
            return authorized;
    }
    return CONCURRENCY_CHANNEL_EFFECTS_VALID;
}
