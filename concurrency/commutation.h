#ifndef MONAD_CONCURRENCY_COMMUTATION_H
#define MONAD_CONCURRENCY_COMMUTATION_H

/*
 * Executable adjacent-swap validation for the Cycle 5.6 dependence oracle.
 * Independence as preservation of enabledness and commuting transitions:
 * https://doi.org/10.1145/1040305.1040315
 */

#include "dependence.h"

typedef enum {
    CONCURRENCY_COMMUTATION_CERTIFIED,
    CONCURRENCY_COMMUTATION_INVALID_ARGUMENT,
    CONCURRENCY_COMMUTATION_INVALID_PROJECTION,
    CONCURRENCY_COMMUTATION_DEPENDENT,
    CONCURRENCY_COMMUTATION_NOT_COENABLED,
    CONCURRENCY_COMMUTATION_DOES_NOT_COMMUTE,
    CONCURRENCY_COMMUTATION_OUT_OF_MEMORY,
    CONCURRENCY_COMMUTATION_INVALID_CERTIFICATE,
} ConcurrencyCommutationStatus;

typedef struct {
    size_t prefix_count;
    ConcurrencyDeterministicEvent task_event;
    ConcurrencyDeterministicEvent channel_event;
    uint64_t forward_state_fingerprint;
    uint64_t reverse_state_fingerprint;
    uint64_t fingerprint;
} ConcurrencyCommutationCertificate;

ConcurrencyCommutationStatus concurrency_deterministic_commute(
    const ConcurrencyDeterministicTrace *trace, size_t prefix_count,
    ConcurrencyCommutationCertificate *certificate);
ConcurrencyCommutationStatus concurrency_deterministic_commutation_verify(
    const ConcurrencyDeterministicTrace *trace,
    const ConcurrencyCommutationCertificate *certificate);

#endif
