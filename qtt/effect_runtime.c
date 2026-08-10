#include "effect_runtime.h"

#include <string.h>

void qtt_effect_runtime_init(QttEffectRuntime *runtime) {
    if (runtime) memset(runtime, 0, sizeof(*runtime));
}

bool qtt_effect_runtime_push(
    QttEffectRuntime *runtime, QttEffectHandlerFrame *frame,
    uint64_t constructor_id, uint64_t capability_id,
    QttAbortiveEffectClause clause, void *environment) {
    if (!runtime || !frame || frame->active || !constructor_id ||
        !capability_id || !clause)
        return false;
    frame->parent = runtime->top;
    frame->constructor_id = constructor_id;
    frame->capability_id = capability_id;
    frame->clause = clause;
    frame->environment = environment;
    frame->active = true;
    runtime->top = frame;
    return true;
}

bool qtt_effect_runtime_pop(
    QttEffectRuntime *runtime, QttEffectHandlerFrame *frame) {
    if (!runtime || !frame || !frame->active || runtime->top != frame)
        return false;
    runtime->top = frame->parent;
    memset(frame, 0, sizeof(*frame));
    return true;
}

QttEffectDispatchResult qtt_effect_runtime_perform_abortive(
    QttEffectRuntime *runtime, uint64_t constructor_id,
    uint64_t capability_id, void *argument, void **output) {
    if (!runtime || !constructor_id || !capability_id || !output)
        return QTT_EFFECT_DISPATCH_INVALID;
    *output = NULL;
    for (QttEffectHandlerFrame *frame = runtime->top;
         frame; frame = frame->parent) {
        if (!frame->active || frame->constructor_id != constructor_id ||
            frame->capability_id != capability_id)
            continue;
        if (!frame->clause) return QTT_EFFECT_DISPATCH_INVALID;
        runtime->dispatch_count++;
        *output = frame->clause(argument, frame->environment);
        return QTT_EFFECT_DISPATCH_HANDLED;
    }
    return QTT_EFFECT_DISPATCH_UNHANDLED;
}
