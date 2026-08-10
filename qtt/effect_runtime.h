#ifndef MONAD_QTT_EFFECT_RUNTIME_H
#define MONAD_QTT_EFFECT_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

typedef void *(*QttAbortiveEffectClause)(void *argument, void *environment);

typedef struct QttEffectHandlerFrame {
    struct QttEffectHandlerFrame *parent;
    uint64_t constructor_id;
    uint64_t capability_id;
    QttAbortiveEffectClause clause;
    void *environment;
    bool active;
} QttEffectHandlerFrame;

typedef struct {
    QttEffectHandlerFrame *top;
    uint64_t dispatch_count;
} QttEffectRuntime;

typedef enum {
    QTT_EFFECT_DISPATCH_HANDLED,
    QTT_EFFECT_DISPATCH_UNHANDLED,
    QTT_EFFECT_DISPATCH_INVALID,
} QttEffectDispatchResult;

void qtt_effect_runtime_init(QttEffectRuntime *runtime);
bool qtt_effect_runtime_push(
    QttEffectRuntime *runtime, QttEffectHandlerFrame *frame,
    uint64_t constructor_id, uint64_t capability_id,
    QttAbortiveEffectClause clause, void *environment);
bool qtt_effect_runtime_pop(
    QttEffectRuntime *runtime, QttEffectHandlerFrame *frame);
QttEffectDispatchResult qtt_effect_runtime_perform_abortive(
    QttEffectRuntime *runtime, uint64_t constructor_id,
    uint64_t capability_id, void *argument, void **output);

#endif
