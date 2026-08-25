#ifndef MONAD_CONCURRENCY_NETWORK_TRACE_H
#define MONAD_CONCURRENCY_NETWORK_TRACE_H

/*
 * Interleaved higher-order channel replay deriving Cycle 4.3 snapshots.
 * Cycle 4.6 additionally requires exact scoped QTT effect-row authority.
 * Higher-order session preservation:
 * http://hdl.handle.net/10451/14108
 * Exceptional asynchronous buffer semantics:
 * https://doi.org/10.1145/3290341
 * Scoped effect capabilities:
 * https://doi.org/10.1145/3731678
 */

#include "effect.h"
#include "network.h"

typedef struct {
    ConcurrencyChannelId channel_id;
    ConcurrencyChannelEndpoint left;
    ConcurrencyChannelEndpoint right;
    size_t capacity;
    uint64_t scope_capability_id;
    uint64_t channel_type_id;
} ConcurrencyNetworkChannel;

typedef struct {
    ConcurrencyNetworkAuthority authority;
    ConcurrencyTaskId owner;
} ConcurrencyNetworkInitialAuthority;

typedef struct {
    ConcurrencyChannelId channel_id;
    ConcurrencyChannelStep channel_step;
} ConcurrencyNetworkTraceStep;

typedef struct {
    const ConcurrencyNetworkChannel *channels;
    size_t channel_count;
    const ConcurrencyNetworkInitialAuthority *authorities;
    size_t authority_count;
    const ConcurrencyNetworkTraceStep *steps;
    size_t step_count;
    const QttEffectSolver *effect_solver;
    const QttEffectRow *effects;
} ConcurrencyNetworkTrace;

typedef struct {
    ConcurrencyNetworkSnapshot snapshot;
    ConcurrencyNetworkCertificate certificate;
} ConcurrencyNetworkDerived;

typedef enum {
    CONCURRENCY_NETWORK_TRACE_CERTIFIED,
    CONCURRENCY_NETWORK_TRACE_INVALID_ARGUMENT,
    CONCURRENCY_NETWORK_TRACE_OUT_OF_MEMORY,
    CONCURRENCY_NETWORK_TRACE_MALFORMED,
    CONCURRENCY_NETWORK_TRACE_UNKNOWN_CHANNEL,
    CONCURRENCY_NETWORK_TRACE_PROTOCOL_ERROR,
    CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR,
    CONCURRENCY_NETWORK_TRACE_CAPACITY_ERROR,
    CONCURRENCY_NETWORK_TRACE_UNSUPPORTED_STEP,
    CONCURRENCY_NETWORK_TRACE_EFFECT_ERROR,
    CONCURRENCY_NETWORK_TRACE_CERTIFICATE_ERROR,
} ConcurrencyNetworkTraceStatus;

ConcurrencyNetworkTraceStep concurrency_network_step(
    ConcurrencyChannelId channel_id, ConcurrencyChannelStep channel_step);
ConcurrencyNetworkTraceStatus concurrency_network_derive(
    const ConcurrencyNetworkTrace *trace, ConcurrencyNetworkDerived *derived);
void concurrency_network_derived_free(ConcurrencyNetworkDerived *derived);

#endif
