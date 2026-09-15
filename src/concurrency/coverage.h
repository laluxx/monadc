#ifndef MONAD_CONCURRENCY_COVERAGE_H
#define MONAD_CONCURRENCY_COVERAGE_H

/*
 * Constructive Mazurkiewicz-class coverage by certified adjacent swaps.
 * Trace equivalence and happens-before in the original DPOR construction:
 * https://doi.org/10.1145/1040305.1040315
 */

#include "exploration.h"

typedef struct {
    size_t max_swaps_per_witness;
} ConcurrencyCoverageLimits;

typedef struct {
    size_t exhaustive_schedule_index;
    size_t representative_schedule_index;
    uint64_t exhaustive_schedule_fingerprint;
    uint64_t representative_schedule_fingerprint;
    size_t *swap_positions;
    size_t swap_count;
} ConcurrencyCoverageWitness;

typedef struct {
    ConcurrencyCoverageLimits limits;
    uint64_t exhaustive_exploration_fingerprint;
    uint64_t reduced_exploration_fingerprint;
    ConcurrencyCoverageWitness *witnesses;
    size_t witness_count;
    size_t covered_schedule_count;
    size_t total_swap_count;
    uint64_t fingerprint;
} ConcurrencyCoverage;

typedef enum {
    CONCURRENCY_COVERAGE_CERTIFIED,
    CONCURRENCY_COVERAGE_INVALID_ARGUMENT,
    CONCURRENCY_COVERAGE_INVALID_EXPLORATION,
    CONCURRENCY_COVERAGE_UNCOVERED_SCHEDULE,
    CONCURRENCY_COVERAGE_LIMIT_EXCEEDED,
    CONCURRENCY_COVERAGE_OUT_OF_MEMORY,
    CONCURRENCY_COVERAGE_INVALID_CERTIFICATE,
} ConcurrencyCoverageStatus;

ConcurrencyCoverageStatus concurrency_deterministic_coverage_build(
    const ConcurrencyDeterministicTrace *trace,
    const ConcurrencyExploration *exhaustive,
    const ConcurrencyExploration *reduced,
    ConcurrencyCoverageLimits limits,
    ConcurrencyCoverage *coverage);
ConcurrencyCoverageStatus concurrency_deterministic_coverage_verify(
    const ConcurrencyDeterministicTrace *trace,
    const ConcurrencyExploration *exhaustive,
    const ConcurrencyExploration *reduced,
    const ConcurrencyCoverage *coverage);
void concurrency_deterministic_coverage_free(
    ConcurrencyCoverage *coverage);

#endif
