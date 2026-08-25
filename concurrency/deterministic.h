#ifndef MONAD_CONCURRENCY_DETERMINISTIC_H
#define MONAD_CONCURRENCY_DETERMINISTIC_H

/*
 * Deterministic interpreter for certified concurrency projections.
 * Systematic concurrency testing and replay (CHESS):
 * https://www.microsoft.com/en-us/research/publication/finding-and-reproducing-heisenbugs-in-concurrent-programs/
 * Dynamic partial-order reduction (future exploration layer):
 * https://doi.org/10.1145/1040305.1040315
 * Cycle 5.2 adds prefix enabledness, not DPOR exploration.
 */

#include "cleanup_bridge.h"

typedef enum {
    CONCURRENCY_DETERMINISTIC_TASK_EVENT,
    CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT,
} ConcurrencyDeterministicEventKind;

typedef struct {
    ConcurrencyDeterministicEventKind kind;
    size_t projection_index;
    uint64_t tick;
} ConcurrencyDeterministicEvent;

typedef struct {
    const ConcurrencyTrace *task_trace;
    const ConcurrencyNetworkTrace *network_trace;
    const ConcurrencyDeterministicEvent *events;
    size_t event_count;
} ConcurrencyDeterministicTrace;

typedef struct {
    size_t event_count;
    uint64_t final_tick;
    uint64_t cleanup_fingerprint;
    uint64_t schedule_fingerprint;
    uint64_t fingerprint;
} ConcurrencyDeterministicCertificate;

typedef enum {
    CONCURRENCY_DETERMINISTIC_ENABLED,
    CONCURRENCY_DETERMINISTIC_PROJECTION_COMPLETE,
    CONCURRENCY_DETERMINISTIC_TASK_NOT_LIVE,
    CONCURRENCY_DETERMINISTIC_TASK_NOT_CLEANING,
    CONCURRENCY_DETERMINISTIC_AWAITS_CLEANUP_DISCHARGE,
    CONCURRENCY_DETERMINISTIC_LOCAL_CHANNEL_BLOCKED,
} ConcurrencyDeterministicBlockReason;

typedef struct {
    bool task_enabled;
    bool channel_enabled;
    ConcurrencyDeterministicBlockReason task_blocked;
    ConcurrencyDeterministicBlockReason channel_blocked;
    ConcurrencyChannelBlockReason local_channel_blocked;
    size_t next_task_index;
    size_t next_channel_index;
} ConcurrencyDeterministicEnabled;

typedef enum {
    CONCURRENCY_DETERMINISTIC_CERTIFIED,
    CONCURRENCY_DETERMINISTIC_INVALID_ARGUMENT,
    CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION,
    CONCURRENCY_DETERMINISTIC_MALFORMED_SCHEDULE,
    CONCURRENCY_DETERMINISTIC_TIME_REGRESSION,
    CONCURRENCY_DETERMINISTIC_CANCEL_OUTSIDE_CLEANUP,
    CONCURRENCY_DETERMINISTIC_EVENT_BLOCKED,
    CONCURRENCY_DETERMINISTIC_INVALID_CERTIFICATE,
} ConcurrencyDeterministicStatus;

ConcurrencyDeterministicEvent concurrency_deterministic_task_event(
    size_t projection_index, uint64_t tick);
ConcurrencyDeterministicEvent concurrency_deterministic_channel_event(
    size_t projection_index, uint64_t tick);
ConcurrencyDeterministicStatus concurrency_deterministic_channel_enabled(
    const ConcurrencyNetworkTrace *trace, size_t candidate_index,
    ConcurrencyChannelMachineResult *result);
ConcurrencyDeterministicStatus concurrency_deterministic_temporal_enabled(
    const ConcurrencyTrace *task_trace,
    const ConcurrencyNetworkTrace *network_trace,
    const ConcurrencyDeterministicEvent *selected, size_t selected_count,
    ConcurrencyDeterministicEvent candidate, bool *enabled,
    ConcurrencyDeterministicBlockReason *blocked);
ConcurrencyDeterministicStatus concurrency_deterministic_enabled(
    const ConcurrencyDeterministicTrace *trace, size_t prefix_count,
    ConcurrencyDeterministicEnabled *enabled);
ConcurrencyDeterministicStatus concurrency_deterministic_run(
    const ConcurrencyDeterministicTrace *trace,
    ConcurrencyDeterministicCertificate *certificate);
ConcurrencyDeterministicStatus concurrency_deterministic_verify(
    const ConcurrencyDeterministicTrace *trace,
    const ConcurrencyDeterministicCertificate *certificate);

#endif
