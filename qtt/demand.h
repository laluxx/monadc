#ifndef MONAD_QTT_DEMAND_H
#define MONAD_QTT_DEMAND_H

#include "core.h"
#include "quantity.h"

/*
 * A demand certificate is tied to one immutable typed-Core tree.  Demand is
 * derived in the free commutative semiring with explicit control-flow choice:
 *
 *   use(x,x) = 1
 *   use(x,e1 ; e2) = use(x,e1) + use(x,e2)
 *   use(x,if c t e) = use(x,c) + choice(use(x,t), use(x,e))
 *
 * Keeping choice symbolic is essential: adding mutually exclusive arms would
 * overstate runtime demand and obscure branch-local last use.
 */
typedef struct QttDemandCertificate QttDemandCertificate;

typedef enum {
    QTT_DEMAND_OK,
    QTT_DEMAND_OUT_OF_MEMORY,
    QTT_DEMAND_UNSUPPORTED_CORE,
} QttDemandError;

typedef enum {
    QTT_DEMAND_VALID,
    QTT_DEMAND_SOURCE_MISMATCH,
} QttDemandValidation;

typedef enum {
    QTT_DEMAND_RULE_ZERO,
    QTT_DEMAND_RULE_VAR,
    QTT_DEMAND_RULE_LET,
    QTT_DEMAND_RULE_IF,
    QTT_DEMAND_RULE_SEQUENCE,
    QTT_DEMAND_RULE_APPLY,
    QTT_DEMAND_RULE_LAMBDA,
    QTT_DEMAND_RULE_WRITE,
    QTT_DEMAND_RULE_BORROW,
} QttDemandRule;

typedef struct QttDemandDerivation {
    QttDemandRule rule;
    const QttCoreNode *source;
    QttCoreVar subject;
    QttQuantity syntactic;
    QttQuantity runtime;
    struct QttDemandDerivation **premises;
    size_t premise_count;
} QttDemandDerivation;

typedef enum {
    QTT_DEMAND_PROOF_VALID,
    QTT_DEMAND_PROOF_SOURCE_MISMATCH,
    QTT_DEMAND_PROOF_RULE_MISMATCH,
    QTT_DEMAND_PROOF_PREMISE_MISMATCH,
    QTT_DEMAND_PROOF_GRADE_MISMATCH,
} QttDemandProofValidation;

QttDemandCertificate *qtt_demand_derive(const QttCoreNode *core,
                                        QttDemandError *error);
QttDemandValidation qtt_demand_validate(
    const QttDemandCertificate *certificate,
    const QttCoreNode *core);
QttDemandDerivation *qtt_demand_prove(
    const QttDemandCertificate *certificate,
    const QttCoreNode *subtree,
    QttCoreVar var,
    QttDemandError *error);
QttDemandProofValidation qtt_demand_check(
    const QttDemandDerivation *derivation,
    const QttCoreNode *subtree,
    QttCoreVar var);
/* Stable structural identity for a derivation node while Core is live. */
uint64_t qtt_demand_derivation_node_fingerprint(
    const QttDemandDerivation *derivation);
void qtt_demand_derivation_free(QttDemandDerivation *derivation);

/* Exact syntactic occurrences in this subtree, saturating at SIZE_MAX. */
size_t qtt_demand_occurrences(const QttDemandCertificate *certificate,
                              const QttCoreNode *subtree,
                              QttCoreVar var);
QttQuantity qtt_demand_runtime(
    const QttDemandCertificate *certificate,
    const QttCoreNode *subtree,
    QttCoreVar var);

/* Canonical symbolic form owned by certificate until the next format call. */
const char *qtt_demand_format(QttDemandCertificate *certificate,
                              const QttCoreNode *subtree,
                              QttCoreVar var);
void qtt_demand_certificate_free(QttDemandCertificate *certificate);

#endif
