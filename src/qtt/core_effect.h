#ifndef MONAD_QTT_CORE_EFFECT_H
#define MONAD_QTT_CORE_EFFECT_H

#include "core.h"
#include "../effects/effect.h"
#include "../effects/constraints.h"

typedef enum {
    QTT_CORE_EFFECT_OK,
    QTT_CORE_EFFECT_OUT_OF_MEMORY,
    QTT_CORE_EFFECT_UNKNOWN_EFFECT,
    QTT_CORE_EFFECT_UNKNOWN_PROFILE,
    QTT_CORE_EFFECT_PROFILE_MISMATCH,
    QTT_CORE_EFFECT_OPERATION_ABSENT,
    QTT_CORE_EFFECT_MULTIPLE_OPERATIONS,
    QTT_CORE_EFFECT_UNSUPPORTED_CONTEXT,
    QTT_CORE_EFFECT_INVALID_CALL_EFFECT,
    QTT_CORE_EFFECT_GRADE_VIOLATION,
    QTT_CORE_EFFECT_SCOPED_ESCAPE,
    QTT_CORE_EFFECT_UNSUPPORTED_SHALLOW,
    QTT_CORE_EFFECT_OPERATION_TYPE_MISMATCH,
    QTT_CORE_EFFECT_INVALID_CORE,
} QttCoreEffectStatus;

typedef struct {
    QttCoreEffectStatus status;
    QttEffectArena *arena;
    QttEffectSolver *solver;
    QttEffectRow *effects;
    uint64_t fingerprint;
    QttEffectConstraintSet *constraints;
    QttEffectConstraintCertificate *constraint_certificate;
    QttEffectConstraintResult constraint_result;
    uint64_t constraint_fingerprint;
    /* Replay material is retained only for a successfully handled root. */
    const QttEffectRow *computation_effects;
    const QttEffectRow *clause_effects;
    QttEffectAtom handled_atom;
    QttQuantity continuation_usage;
    QttQuantity capture_allowance;
} QttCoreEffectResult;

typedef enum {
    QTT_CORE_EFFECT_RUNTIME_INVALID,
    QTT_CORE_EFFECT_RUNTIME_ABORTIVE_DIRECT,
    QTT_CORE_EFFECT_RUNTIME_ONE_SHOT_LINEAR,
    QTT_CORE_EFFECT_RUNTIME_UNSUPPORTED_MULTI_SHOT,
} QttCoreEffectRuntimePolicy;

QttCoreEffectResult qtt_core_effect_elaborate(
    QttCoreNode *root, QttQuantity capture_allowance);
bool qtt_core_effect_verify_handle(
    const QttCoreNode *handle, const QttCoreEffectResult *result);
bool qtt_core_effect_verify_constraints(
    const QttCoreEffectResult *result);
QttCoreEffectRuntimePolicy qtt_core_effect_runtime_policy(
    const QttCoreNode *handle);
uint64_t qtt_core_effect_authority_fingerprint(
    const QttCoreNode *node);
void qtt_core_effect_result_free(QttCoreEffectResult *result);

#endif
