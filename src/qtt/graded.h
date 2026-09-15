#ifndef MONAD_QTT_GRADED_H
#define MONAD_QTT_GRADED_H

#include "demand.h"
#include "type_identity.h"

/*
 * Executable graded typing judgment for the current typed Core:
 *
 *   Γ ⊢ e : A ▷ Δ
 *
 * Γ assigns each free BinderId a canonical type and an allowed usage.
 * Δ is the path-sensitive usage derived for e.  The judgment is valid when
 * the Core result has canonical type A and Δ(x) <= Γ(x) for every binder.
 */
typedef struct {
    QttCoreVar var;
    QttTypeId type_id;
    QttQuantity allowance;
} QttGradedBinder;

typedef struct {
    const QttGradedBinder *binders;
    size_t count;
} QttGradedContext;

typedef struct QttGradedCertificate QttGradedCertificate;

typedef enum {
    QTT_GRADED_OK,
    QTT_GRADED_OUT_OF_MEMORY,
    QTT_GRADED_INVALID_CONTEXT,
    QTT_GRADED_TYPE_MISMATCH,
    QTT_GRADED_USAGE_EXCEEDED,
    QTT_GRADED_DEMAND_ERROR,
} QttGradedError;

typedef enum {
    QTT_GRADED_PROOF_VALID,
    QTT_GRADED_PROOF_SOURCE_MISMATCH,
    QTT_GRADED_PROOF_TYPE_MISMATCH,
    QTT_GRADED_PROOF_CONTEXT_MISMATCH,
    QTT_GRADED_PROOF_DEMAND_MISMATCH,
} QttGradedProofValidation;

QttGradedCertificate *qtt_graded_check(
    const QttCoreNode *core,
    QttTypeId expected_type,
    const QttGradedContext *context,
    QttTypeArena *types,
    QttGradedError *error);
QttGradedProofValidation qtt_graded_validate(
    const QttGradedCertificate *certificate,
    const QttCoreNode *core,
    QttTypeId expected_type,
    const QttGradedContext *context,
    QttTypeArena *types);
QttQuantity qtt_graded_observed(
    const QttGradedCertificate *certificate, QttCoreVar var);
bool qtt_graded_is_erased(
    const QttGradedCertificate *certificate, QttCoreVar var);
void qtt_graded_certificate_free(QttGradedCertificate *certificate);

#endif
