#ifndef MONAD_QTT_SEMANTIC_CLOSURE_H
#define MONAD_QTT_SEMANTIC_CLOSURE_H

#include "closure.h"
#include "semantic_ir.h"

typedef enum {
    QTT_SEMANTIC_CLOSURE_OK,
    QTT_SEMANTIC_CLOSURE_INVALID_IR,
    QTT_SEMANTIC_CLOSURE_NOT_FOUND,
    QTT_SEMANTIC_CLOSURE_FIELD_MISMATCH,
} QttSemanticClosureError;

/*
 * Check that a physical layout plan is exactly the realization of one
 * verified Semantic-IR closure record. Fingerprints are checked, but matching
 * source/type structure remains the authority.
 */
QttSemanticClosureError qtt_semantic_closure_verify_plan(
    QttSemanticFunction *semantic, const QttCoreNode *lambda,
    const QttClosurePlan *plan);

#endif
