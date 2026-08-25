#ifndef MONAD_CONCURRENCY_NETWORK_H
#define MONAD_CONCURRENCY_NETWORK_H

/*
 * Whole-network cancellation-closure evidence for higher-order sessions.
 * Exceptional asynchronous sessions and the E-Zap closure rule:
 * https://doi.org/10.1145/3290341
 * Linear channel transmission as logical cut:
 * https://homepages.inf.ed.ac.uk/wadler/papers/propositions-as-sessions/propositions-as-sessions.pdf
 * Quantitative authority and exact use:
 * https://bentnib.org/quantitative-type-theory.pdf
 * Proof production separated from independent replay:
 * https://people.eecs.berkeley.edu/~necula/Papers/pcc.pdf
 */

#include "../qtt/place.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t module_id;
    uint64_t endpoint_id;
} ConcurrencyNetworkEndpointId;

typedef struct {
    ConcurrencyNetworkEndpointId endpoint;
    QttPlace place;
    QttQuantity quantity;
    uint64_t protocol_type_id;
} ConcurrencyNetworkAuthority;

/* container's queue slot uniquely owns contained while the message is live. */
typedef struct {
    ConcurrencyNetworkEndpointId container;
    ConcurrencyNetworkEndpointId contained;
    uint64_t slot;
} ConcurrencyNetworkQueueEdge;

typedef struct {
    const ConcurrencyNetworkAuthority *authorities;
    size_t authority_count;
    const ConcurrencyNetworkQueueEdge *queue_edges;
    size_t queue_edge_count;
    const ConcurrencyNetworkEndpointId *cancelled_roots;
    size_t cancelled_root_count;
} ConcurrencyNetworkSnapshot;

typedef struct {
    ConcurrencyNetworkEndpointId endpoint;
    size_t parent_index;
    size_t distance;
    QttQuantity grade;
} ConcurrencyNetworkCancellationNode;

typedef struct {
    ConcurrencyNetworkCancellationNode *nodes;
    size_t node_count;
    size_t root_count;
    QttQuantity total_grade;
    uint64_t snapshot_fingerprint;
    uint64_t fingerprint;
} ConcurrencyNetworkCertificate;

typedef enum {
    CONCURRENCY_NETWORK_CERTIFIED,
    CONCURRENCY_NETWORK_INVALID_ARGUMENT,
    CONCURRENCY_NETWORK_OUT_OF_MEMORY,
    CONCURRENCY_NETWORK_MALFORMED,
    CONCURRENCY_NETWORK_UNKNOWN_ENDPOINT,
    CONCURRENCY_NETWORK_OVERLAPPING_AUTHORITY,
    CONCURRENCY_NETWORK_NON_LINEAR_AUTHORITY,
    CONCURRENCY_NETWORK_DUPLICATE_SLOT,
    CONCURRENCY_NETWORK_DUPLICATE_IN_FLIGHT_OWNER,
    CONCURRENCY_NETWORK_INVALID_GRADE,
    CONCURRENCY_NETWORK_INVALID_SHAPE,
    CONCURRENCY_NETWORK_INVALID_FINGERPRINT,
} ConcurrencyNetworkStatus;

ConcurrencyNetworkStatus concurrency_network_build(
    const ConcurrencyNetworkSnapshot *snapshot,
    ConcurrencyNetworkCertificate *certificate);
ConcurrencyNetworkStatus concurrency_network_verify(
    const ConcurrencyNetworkSnapshot *snapshot,
    const ConcurrencyNetworkCertificate *certificate);
void concurrency_network_free(ConcurrencyNetworkCertificate *certificate);

#endif
