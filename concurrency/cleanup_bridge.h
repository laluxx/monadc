#ifndef MONAD_CONCURRENCY_CLEANUP_BRIDGE_H
#define MONAD_CONCURRENCY_CLEANUP_BRIDGE_H

/*
 * Proof bridge from exceptional-session E-Zap closure to structured cleanup.
 * Exceptional asynchronous sessions:
 * https://doi.org/10.1145/3290341
 * Structured task lifetime and cancellation:
 * https://openjdk.org/jeps/505
 * Effect handlers with reliable finalization:
 * https://www.microsoft.com/en-us/research/publication/algebraic-effect-handlers-resources-deep-finalization/
 * Proof production separated from replay:
 * https://people.eecs.berkeley.edu/~necula/Papers/pcc.pdf
 */

#include "network_trace.h"

typedef struct {
    ConcurrencyNetworkEndpointId endpoint;
    ConcurrencyTaskId cleanup_task;
    size_t root_index;
    QttQuantity grade;
} ConcurrencyCleanupDischarge;

typedef struct {
    ConcurrencyCleanupDischarge *discharges;
    size_t discharge_count;
    size_t root_count;
    QttQuantity total_grade;
    uint64_t network_fingerprint;
    uint64_t fingerprint;
} ConcurrencyCleanupBridgeCertificate;

typedef enum {
    CONCURRENCY_CLEANUP_BRIDGE_CERTIFIED,
    CONCURRENCY_CLEANUP_BRIDGE_INVALID_ARGUMENT,
    CONCURRENCY_CLEANUP_BRIDGE_OUT_OF_MEMORY,
    CONCURRENCY_CLEANUP_BRIDGE_INVALID_NETWORK,
    CONCURRENCY_CLEANUP_BRIDGE_INVALID_TASK_TRACE,
    CONCURRENCY_CLEANUP_BRIDGE_MISSING_CLEANUP,
    CONCURRENCY_CLEANUP_BRIDGE_INVALID_DISCHARGE,
} ConcurrencyCleanupBridgeStatus;

ConcurrencyCleanupBridgeStatus concurrency_cleanup_bridge_build(
    const ConcurrencyNetworkTrace *network_trace,
    const ConcurrencyTrace *task_trace,
    ConcurrencyCleanupBridgeCertificate *certificate);
ConcurrencyCleanupBridgeStatus concurrency_cleanup_bridge_verify(
    const ConcurrencyNetworkTrace *network_trace,
    const ConcurrencyTrace *task_trace,
    const ConcurrencyCleanupBridgeCertificate *certificate);
void concurrency_cleanup_bridge_free(
    ConcurrencyCleanupBridgeCertificate *certificate);

#endif
