#ifndef MONAD_QTT_SIGNATURE_H
#define MONAD_QTT_SIGNATURE_H

#include "demand.h"
#include "effect.h"
#include "graded.h"
#include "resource.h"
#include "type_identity.h"

typedef enum {
    QTT_OWNERSHIP_ERASED,
    QTT_OWNERSHIP_BORROWED,
    QTT_OWNERSHIP_CONSUMED,
    QTT_OWNERSHIP_SHARED,
} QttOwnershipMode;

/* Generative nominal authority is orthogonal to structural QttTypeId. Zero is
 * the absence of nominal authority; foreign representations require both
 * components. Runtime authorities are intentionally not serialized. */
typedef struct {
    uint64_t domain;
    uint64_t identity;
} QttNominalAuthority;

static inline bool qtt_nominal_authority_equal(
    QttNominalAuthority left, QttNominalAuthority right) {
    return left.domain != 0 && left.identity != 0 &&
           left.domain == right.domain && left.identity == right.identity;
}

typedef struct {
    QttCoreVar var;
    /* Declared/inferred allowance checked by the graded judgment. */
    QttQuantity quantity;
    /* Demand proven for this particular function body. */
    QttQuantity observed;
    QttOwnershipMode mode;
    const Type *type;
    QttTypeId type_id;
    QttRepresentation representation;
    QttNominalAuthority nominal_authority;
} QttParameterContract;

typedef enum {
    QTT_RESULT_IMMEDIATE,
    QTT_RESULT_OWNED,
    QTT_RESULT_BORROWED,
    QTT_RESULT_SHARED,
    QTT_RESULT_UNKNOWN,
} QttResultMode;

typedef enum {
    QTT_RESULT_ORIGIN_IMMEDIATE,
    QTT_RESULT_ORIGIN_STATIC,
    QTT_RESULT_ORIGIN_TRANSFERRED,
    QTT_RESULT_ORIGIN_FRESH,
    QTT_RESULT_ORIGIN_UNKNOWN,
} QttResultOrigin;

typedef struct {
    QttResultMode mode;
    QttResultOrigin origin;
    const Type *type;
    QttTypeId type_id;
    QttRepresentation representation;
    QttNominalAuthority nominal_authority;
} QttResultContract;

typedef struct QttFunctionSignature {
    QttParameterContract *parameters;
    size_t parameter_count;
    const Type *result_type;
    QttResultContract result;
    const QttCoreNode *source;
    QttDemandCertificate *demand;
    QttGradedCertificate *graded;
    QttEffectArena *effect_arena;
    QttEffectSolver *effect_solver;
    QttEffectRow *latent_effects;
    bool effects_complete;
    /* The row was closed against a callable environment/fixed point. */
    bool effects_environment_solved;
    uint64_t effect_fingerprint;
    uint64_t contract_fingerprint;
} QttFunctionSignature;

typedef enum {
    QTT_SIGNATURE_OK,
    QTT_SIGNATURE_OUT_OF_MEMORY,
    QTT_SIGNATURE_NOT_A_FUNCTION,
} QttSignatureError;

typedef enum {
    QTT_SIGNATURE_VALID,
    QTT_SIGNATURE_SOURCE_MISMATCH,
    QTT_SIGNATURE_EFFECT_MISMATCH,
} QttSignatureValidation;

typedef enum {
    QTT_SIGNATURE_CANONICAL,
    QTT_SIGNATURE_MISSING_TYPE,
    QTT_SIGNATURE_GRADED_INVALID,
    QTT_SIGNATURE_CANONICAL_OUT_OF_MEMORY,
} QttSignatureCanonicalization;

typedef const QttFunctionSignature *(*QttSignatureEffectLookup)(
    void *context, uint64_t module_id, const char *name);

typedef enum {
    QTT_SIGNATURE_EFFECT_REFRESH_UNCHANGED,
    QTT_SIGNATURE_EFFECT_REFRESH_CHANGED,
    QTT_SIGNATURE_EFFECT_REFRESH_OUT_OF_MEMORY,
} QttSignatureEffectRefresh;

QttFunctionSignature *qtt_signature_derive(
    const QttCoreNode *lambda,
    QttSignatureError *error);
QttSignatureValidation qtt_signature_validate(
    const QttFunctionSignature *signature,
    const QttCoreNode *lambda);
QttSignatureValidation qtt_signature_validate_in_effect_env(
    const QttFunctionSignature *signature,
    const QttCoreNode *lambda, uint64_t module_id,
    QttSignatureEffectLookup lookup, void *context);
QttSignatureCanonicalization qtt_signature_canonicalize(
    QttFunctionSignature *signature,
    QttTypeArena *arena);
QttGradedProofValidation qtt_signature_validate_grades(
    const QttFunctionSignature *signature,
    QttTypeArena *arena);
QttSignatureEffectRefresh qtt_signature_refresh_effects(
    QttFunctionSignature *signature, uint64_t module_id,
    QttSignatureEffectLookup lookup, void *context);
uint64_t qtt_signature_contract_fingerprint(
    const QttFunctionSignature *signature);
/* Trusted runtime constructors whose ABI returns a fresh owned capability. */
bool qtt_signature_fresh_result_primitive(const char *name);
void qtt_signature_free(QttFunctionSignature *signature);

#endif
