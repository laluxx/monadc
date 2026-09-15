#ifndef MONAD_CONCURRENCY_PROCESS_COMMUTATION_H
#define MONAD_CONCURRENCY_PROCESS_COMMUTATION_H

/*
 * Process-keyed adjacent commutation, reusing the Cycle 5 dependence and
 * diamond oracles rather than defining another effect/authority semantics.
 * Original dynamic partial-order reduction:
 * https://doi.org/10.1145/1040305.1040315
 * Source sets and optimal DPOR:
 * https://doi.org/10.1145/3073408
 */

#include "commutation.h"
#include "process.h"

typedef struct {
    size_t prefix_count;
    uint64_t prefix_fingerprint;
    uint64_t frontier_fingerprint;
    ConcurrencyProcessId left_process;
    ConcurrencyProcessId right_process;
    size_t left_local_index;
    size_t right_local_index;
    ConcurrencyDeterministicEvent left_event;
    ConcurrencyDeterministicEvent right_event;
    ConcurrencyDependence dependence;
    ConcurrencyCommutationCertificate binary;
    uint64_t fingerprint;
} ConcurrencyProcessCommutationCertificate;

typedef enum {
    CONCURRENCY_PROCESS_COMMUTATION_CERTIFIED,
    CONCURRENCY_PROCESS_COMMUTATION_INVALID_ARGUMENT,
    CONCURRENCY_PROCESS_COMMUTATION_INVALID_PROGRAM,
    CONCURRENCY_PROCESS_COMMUTATION_SAME_PROCESS,
    CONCURRENCY_PROCESS_COMMUTATION_NOT_COENABLED,
    CONCURRENCY_PROCESS_COMMUTATION_DEPENDENT,
    CONCURRENCY_PROCESS_COMMUTATION_DOES_NOT_COMMUTE,
    CONCURRENCY_PROCESS_COMMUTATION_OUT_OF_MEMORY,
    CONCURRENCY_PROCESS_COMMUTATION_INVALID_CERTIFICATE,
} ConcurrencyProcessCommutationStatus;

ConcurrencyProcessCommutationStatus concurrency_process_commute(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *prefix,
    ConcurrencyProcessId left, ConcurrencyProcessId right,
    ConcurrencyProcessCommutationCertificate *certificate);
ConcurrencyProcessCommutationStatus concurrency_process_commutation_verify(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *prefix,
    const ConcurrencyProcessCommutationCertificate *certificate);

#endif
