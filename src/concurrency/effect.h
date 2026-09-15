#ifndef MONAD_CONCURRENCY_EFFECT_H
#define MONAD_CONCURRENCY_EFFECT_H

/*
 * QTT effect-row evidence for certified concurrency traces.
 * Algebraic effects and handlers:
 * https://arxiv.org/abs/1312.1399
 * Row-polymorphic effects with duplicate labels:
 * https://arxiv.org/abs/1406.2061
 * Scoped operations as resource allocation/consumption:
 * https://doi.org/10.1145/3731678
 * Deep finalization across non-resuming operations:
 * https://www.microsoft.com/en-us/research/publication/algebraic-effect-handlers-resources-deep-finalization/
 */

#include "kernel.h"
#include "channel.h"
#include "../effects/effect.h"

typedef enum {
    CONCURRENCY_EFFECT_SPAWN,
    CONCURRENCY_EFFECT_CANCEL,
    CONCURRENCY_EFFECT_CHECKPOINT,
    CONCURRENCY_EFFECT_FAIL,
    CONCURRENCY_EFFECT_CLEANUP_FAIL,
    CONCURRENCY_EFFECT_CLEANUP,
    CONCURRENCY_EFFECT_JOIN,
} ConcurrencyEffectOperation;

typedef enum {
    CONCURRENCY_EFFECTS_VALID,
    CONCURRENCY_EFFECT_INVALID_ARGUMENT,
    CONCURRENCY_EFFECT_INVALID_TRACE,
    CONCURRENCY_EFFECT_MISSING,
} ConcurrencyEffectVerification;

QttEffectAtom concurrency_effect_atom(
    ConcurrencyEffectOperation operation,
    uint64_t scope_capability_id, uint64_t task_type_id);

ConcurrencyEffectVerification concurrency_verify_trace_effects(
    const ConcurrencyTrace *trace, const QttEffectSolver *solver,
    const QttEffectRow *effects,
    uint64_t scope_capability_id, uint64_t task_type_id);

typedef enum {
    CONCURRENCY_CHANNEL_EFFECT_SEND,
    CONCURRENCY_CHANNEL_EFFECT_RECEIVE,
    CONCURRENCY_CHANNEL_EFFECT_CLOSE,
    CONCURRENCY_CHANNEL_EFFECT_CANCEL,
} ConcurrencyChannelEffectOperation;

typedef enum {
    CONCURRENCY_CHANNEL_EFFECTS_VALID,
    CONCURRENCY_CHANNEL_EFFECT_INVALID_ARGUMENT,
    CONCURRENCY_CHANNEL_EFFECT_INVALID_TRACE,
    CONCURRENCY_CHANNEL_EFFECT_MISSING,
} ConcurrencyChannelEffectVerification;

QttEffectAtom concurrency_channel_effect_atom(
    ConcurrencyChannelEffectOperation operation,
    uint64_t scope_capability_id, uint64_t channel_type_id);

ConcurrencyChannelEffectVerification concurrency_authorize_channel_step(
    ConcurrencyChannelStepKind step, const QttEffectSolver *solver,
    const QttEffectRow *effects,
    uint64_t scope_capability_id, uint64_t channel_type_id);

ConcurrencyChannelEffectVerification concurrency_verify_channel_effects(
    const ConcurrencyChannelTrace *trace, const QttEffectSolver *solver,
    const QttEffectRow *effects,
    uint64_t scope_capability_id, uint64_t channel_type_id);

#endif
