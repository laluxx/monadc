#ifndef MONAD_QTT_COHERENCE_H
#define MONAD_QTT_COHERENCE_H

#include "core.h"
#include "quantity.h"
#include "semantic_ir.h"

/*
 * Agreement checker for the intersection of the two QTT generations.
 *
 * This is deliberately not a translation that pretends every construct has
 * one meaning already.  It compares typed-Core demand with symbolic usage on
 * the fragment where their rules coincide and names the first semantic
 * boundary elsewhere.
 */
typedef enum {
    QTT_COHERENCE_AGREES,
    QTT_COHERENCE_DISAGREES,
    QTT_COHERENCE_OUTSIDE_COMMON_FRAGMENT,
    QTT_COHERENCE_INVALID_INPUT,
    QTT_COHERENCE_OUT_OF_MEMORY,
} QttCoherenceStatus;

typedef enum {
    QTT_COHERENCE_BOUNDARY_NONE,
    QTT_COHERENCE_BOUNDARY_APPLICATION,
} QttCoherenceBoundary;

typedef struct {
    QttCoherenceStatus status;
    QttCoherenceBoundary boundary;
    const QttCoreNode *offending;
    QttCoreVar subject;
    QttQuantity legacy;
    QttQuantity symbolic;
    size_t checked_binder_count;
} QttCoherenceReport;

QttCoherenceReport qtt_coherence_check(
    const QttCoreNode *core,
    const QttCoreVar *subjects,
    size_t subject_count);

/*
 * Migration entry point: compare the legacy certificate with quantitative
 * evidence already owned and verified by Semantic IR.  This path does not
 * reconstruct the symbolic interpretation.
 */
QttCoherenceReport qtt_coherence_check_semantic(
    QttSemanticFunction *function,
    const QttCoreNode *core,
    const QttCoreVar *subjects,
    size_t subject_count);

const char *qtt_coherence_boundary_name(QttCoherenceBoundary boundary);

#endif
