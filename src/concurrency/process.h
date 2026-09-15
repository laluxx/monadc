#ifndef MONAD_CONCURRENCY_PROCESS_H
#define MONAD_CONCURRENCY_PROCESS_H

/*
 * Process-keyed operational streams for deterministic concurrency replay.
 * Processes are explicit identities; a schedule is a linear extension of
 * their local orders.  The current checker lowers that extension into the
 * Cycle 5 binary oracle and therefore does not yet claim N-process DPOR.
 *
 * Event structures and inter-process semantics:
 * https://doi.org/10.1007/3-540-17906-2_31
 * Dynamic partial-order reduction:
 * https://doi.org/10.1145/1040305.1040315
 */

#include "deterministic.h"

typedef struct {
    uint64_t module_id;
    uint64_t process_id;
} ConcurrencyProcessId;

typedef struct {
    ConcurrencyDeterministicEventKind kind;
    size_t projection_index;
} ConcurrencyProcessEvent;

typedef struct {
    ConcurrencyProcessId id;
    const ConcurrencyProcessEvent *events;
    size_t event_count;
} ConcurrencyProcess;

typedef struct {
    const ConcurrencyProcess *processes;
    size_t process_count;
} ConcurrencyProcessProgram;

typedef struct {
    ConcurrencyProcessId process;
    size_t local_index;
    uint64_t tick;
} ConcurrencyProcessChoice;

typedef struct {
    const ConcurrencyProcessChoice *choices;
    size_t choice_count;
} ConcurrencyProcessSchedule;

typedef struct {
    size_t process_count;
    size_t event_count;
    uint64_t program_fingerprint;
    uint64_t schedule_fingerprint;
    ConcurrencyDeterministicCertificate deterministic;
    uint64_t fingerprint;
} ConcurrencyProcessCertificate;

typedef enum {
    CONCURRENCY_PROCESS_FRONTIER_ENABLED,
    CONCURRENCY_PROCESS_STREAM_COMPLETE,
    CONCURRENCY_PROCESS_PREDECESSOR_PENDING,
    CONCURRENCY_PROCESS_SEMANTICALLY_BLOCKED,
} ConcurrencyProcessBlockReason;

typedef struct {
    ConcurrencyProcessId process;
    size_t next_local_index;
    bool enabled;
    ConcurrencyProcessBlockReason blocked;
    ConcurrencyDeterministicBlockReason binary_reason;
    ConcurrencyDeterministicEventKind next_kind;
    size_t next_projection_index;
} ConcurrencyProcessFrontierEntry;

typedef struct {
    ConcurrencyProcessFrontierEntry *entries;
    size_t process_count;
    size_t enabled_count;
    size_t prefix_count;
    uint64_t program_fingerprint;
    uint64_t prefix_fingerprint;
    ConcurrencyDeterministicEnabled binary;
    uint64_t fingerprint;
} ConcurrencyProcessFrontier;

typedef enum {
    CONCURRENCY_PROCESS_CERTIFIED,
    CONCURRENCY_PROCESS_INVALID_ARGUMENT,
    CONCURRENCY_PROCESS_INVALID_PROGRAM,
    CONCURRENCY_PROCESS_MALFORMED_SCHEDULE,
    CONCURRENCY_PROCESS_INVALID_PROJECTION,
    CONCURRENCY_PROCESS_OUT_OF_MEMORY,
    CONCURRENCY_PROCESS_INVALID_CERTIFICATE,
} ConcurrencyProcessStatus;

ConcurrencyProcessEvent concurrency_process_event(
    ConcurrencyDeterministicEventKind kind, size_t projection_index);
ConcurrencyProcessChoice concurrency_process_choice(
    ConcurrencyProcessId process, size_t local_index, uint64_t tick);
ConcurrencyProcessStatus concurrency_process_run(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *schedule,
    ConcurrencyProcessCertificate *certificate);
ConcurrencyProcessStatus concurrency_process_verify(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *schedule,
    const ConcurrencyProcessCertificate *certificate);
ConcurrencyProcessStatus concurrency_process_frontier_build(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *prefix,
    ConcurrencyProcessFrontier *frontier);
ConcurrencyProcessStatus concurrency_process_frontier_verify(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *prefix,
    const ConcurrencyProcessFrontier *frontier);
void concurrency_process_frontier_free(ConcurrencyProcessFrontier *frontier);

#endif
