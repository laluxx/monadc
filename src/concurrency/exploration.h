#ifndef MONAD_CONCURRENCY_EXPLORATION_H
#define MONAD_CONCURRENCY_EXPLORATION_H

/*
 * Finite binary source-set exploration over certified task/channel prefixes.
 * Source sets and optimal DPOR:
 * https://doi.org/10.1145/3073408
 * Original dynamic backtracking construction:
 * https://doi.org/10.1145/1040305.1040315
 *
 * The current specialization has two internally ordered processes.  It does
 * not claim wakeup-tree optimality for a future N-process runtime.
 */

#include "commutation.h"

typedef enum {
    CONCURRENCY_EXPLORATION_EXHAUSTIVE,
    CONCURRENCY_EXPLORATION_CERTIFIED_SOURCE,
} ConcurrencyExplorationMode;

typedef struct {
    size_t max_schedules;
} ConcurrencyExplorationLimits;

typedef struct {
    ConcurrencyDeterministicEvent *events;
    size_t event_count;
    ConcurrencyDeterministicCertificate certificate;
} ConcurrencyExploredSchedule;

typedef struct {
    ConcurrencyExplorationMode mode;
    ConcurrencyExplorationLimits limits;
    ConcurrencyExploredSchedule *schedules;
    size_t schedule_count;
    size_t branch_count;
    size_t commutation_count;
    size_t pruned_choice_count;
    size_t fallback_branch_count;
    uint64_t fingerprint;
} ConcurrencyExploration;

typedef enum {
    CONCURRENCY_EXPLORATION_CERTIFIED,
    CONCURRENCY_EXPLORATION_INVALID_ARGUMENT,
    CONCURRENCY_EXPLORATION_INVALID_PROJECTION,
    CONCURRENCY_EXPLORATION_LIMIT_EXCEEDED,
    CONCURRENCY_EXPLORATION_OUT_OF_MEMORY,
    CONCURRENCY_EXPLORATION_DEADLOCK,
    CONCURRENCY_EXPLORATION_INVALID_CERTIFICATE,
} ConcurrencyExplorationStatus;

ConcurrencyExplorationStatus concurrency_deterministic_explore(
    const ConcurrencyDeterministicTrace *trace,
    ConcurrencyExplorationMode mode, ConcurrencyExplorationLimits limits,
    ConcurrencyExploration *exploration);
ConcurrencyExplorationStatus concurrency_deterministic_exploration_verify(
    const ConcurrencyDeterministicTrace *trace,
    const ConcurrencyExploration *exploration);
void concurrency_deterministic_exploration_free(
    ConcurrencyExploration *exploration);

#endif
