#ifndef MONAD_CONCURRENCY_DEPENDENCE_H
#define MONAD_CONCURRENCY_DEPENDENCE_H

/*
 * Conservative semantic dependence for deterministic schedule exploration.
 * Original DPOR (Flanagan and Godefroid, POPL 2005):
 * https://doi.org/10.1145/1040305.1040315
 * Source sets and optimal DPOR (Abdulla et al., JACM 2017):
 * https://doi.org/10.1145/3073408
 *
 * This cycle defines the dependence oracle only.  It does not yet claim a
 * persistent/source-set algorithm or optimal exploration.
 */

#include "deterministic.h"

typedef enum {
    CONCURRENCY_DEPENDENCE_NONE = 0,
    CONCURRENCY_DEPENDENCE_TASK_ORDER = 1u << 0,
    CONCURRENCY_DEPENDENCE_TASK_LIFETIME = 1u << 1,
    CONCURRENCY_DEPENDENCE_CHANNEL = 1u << 2,
    CONCURRENCY_DEPENDENCE_AUTHORITY = 1u << 3,
    CONCURRENCY_DEPENDENCE_CLEANUP = 1u << 4,
    CONCURRENCY_DEPENDENCE_EFFECT = 1u << 5,
} ConcurrencyDependenceReason;

typedef struct {
    bool dependent;
    uint32_t reasons;
} ConcurrencyDependence;

ConcurrencyDeterministicStatus concurrency_deterministic_dependence(
    const ConcurrencyDeterministicTrace *trace,
    ConcurrencyDeterministicEvent left,
    ConcurrencyDeterministicEvent right,
    ConcurrencyDependence *result);

#endif
