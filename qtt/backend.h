#ifndef MONAD_QTT_BACKEND_H
#define MONAD_QTT_BACKEND_H

#include "drop.h"
#include "core_effect.h"
#include "effect_runtime.h"

typedef enum {
    QTT_BACKEND_MATERIALIZE_STATIC_COPY,
    QTT_BACKEND_MATERIALIZE_OWNED_RESULT,
    QTT_BACKEND_MATERIALIZE_LAYOUT_LITERAL,
    QTT_BACKEND_MATERIALIZE_UNIQUE_CLOSURE,
} QttBackendMaterialization;

typedef struct QttBackendCleanup {
    QttCoreVar resource;
    QttDestructorId destructor;
    QttRepresentation representation;
    QttBackendMaterialization materialization;
    size_t binding_index;
    bool destroy;
    QttPlace *evacuated;
    size_t evacuated_count;
    uint64_t closure_moved_mask;
} QttBackendCleanup;

typedef struct {
    QttBackendCleanup *items;
    size_t count;
    /* Every item also matched a valid ownership-ANF alloc/drop proof. */
    bool ownership_anf_certified;
} QttBackendCleanupPlan;

typedef struct {
    QttCoreVar resource;
    QttDestructorId destructor;
    QttRepresentation representation;
    QttBackendMaterialization initializer_materialization;
    QttBackendMaterialization replacement_materialization;
    size_t binding_index;
} QttBackendReplacement;

typedef struct {
    QttBackendReplacement *items;
    size_t count;
} QttBackendReplacementPlan;

typedef enum {
    QTT_BACKEND_EFFECT_OK,
    QTT_BACKEND_EFFECT_INVALID_CORE,
    QTT_BACKEND_EFFECT_INVALID_PROOF,
    QTT_BACKEND_EFFECT_UNSUPPORTED_POLICY,
    QTT_BACKEND_EFFECT_EFFECTFUL_CONTEXT,
} QttBackendEffectStatus;

typedef struct {
    uint64_t constructor_id;
    uint64_t capability_id;
    uint64_t authority_fingerprint;
    uint64_t proof_fingerprint;
    uint64_t constraint_fingerprint;
    const QttCoreNode *operation;
    const QttCoreNode *clause;
} QttBackendAbortiveHandlerPlan;

QttBackendEffectStatus qtt_backend_certify_abortive_handler(
    const QttCoreNode *handle, const QttCoreEffectResult *effects,
    QttBackendAbortiveHandlerPlan *plan);
QttEffectDispatchResult qtt_backend_execute_abortive_handler(
    const QttCoreNode *handle, const QttCoreEffectResult *effects,
    const QttBackendAbortiveHandlerPlan *plan,
    QttEffectRuntime *runtime, QttAbortiveEffectClause clause,
    void *environment, void *argument, void **output);

/*
 * Certify the first production backend fragment: dead owned String literals
 * in a lexical `with`.  Items are returned in destruction order.  The result
 * is safe to consume only when this function succeeds; unsupported source
 * remains outside authoritative QTT code generation.
 */
bool qtt_backend_certify_string_cleanups(
    AST *source, uint64_t module_id,
    QttBackendCleanupPlan *cleanups);
bool qtt_backend_certify_string_cleanups_in_env(
    AST *source, const struct QttSignatureEnv *signatures,
    uint64_t module_id, QttBackendCleanupPlan *cleanups);
bool qtt_backend_certify_string_lifetimes_in_env(
    AST *source, const struct QttSignatureEnv *signatures,
    uint64_t module_id, QttBackendCleanupPlan *lifetimes);
void qtt_backend_cleanup_plan_free(QttBackendCleanupPlan *cleanups);

/*
 * Certify owned String root replacement for production code generation.
 * Each item corresponds, in source/resource order, to one atomic REPLACE.
 * The backend may destroy the displaced payload only while consuming this
 * evidence; unsupported writes retain the legacy conservative store.
 */
bool qtt_backend_certify_string_replacements_in_env(
    AST *source, const struct QttSignatureEnv *signatures,
    uint64_t module_id, QttBackendReplacementPlan *replacements);
void qtt_backend_replacement_plan_free(
    QttBackendReplacementPlan *replacements);

#endif
