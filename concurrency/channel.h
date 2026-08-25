#ifndef MONAD_CONCURRENCY_CHANNEL_H
#define MONAD_CONCURRENCY_CHANNEL_H

/*
 * Independently checked binary session transitions for linear endpoints.
 * Binary session fidelity (Honda, Vasconcelos, Kubo):
 * https://doi.org/10.1007/BFb0053567
 * Corrected higher-order session metatheory (Vasconcelos, Yoshida):
 * http://hdl.handle.net/10451/14108
 * Explicit cancellation for asynchronous functional sessions:
 * https://doi.org/10.1145/3290341
 * QTT grades are reused as endpoint-use evidence:
 * https://bentnib.org/quantitative-type-theory.pdf
 * Linear-logic account of higher-order channel transmission:
 * https://homepages.inf.ed.ac.uk/wadler/papers/propositions-as-sessions/propositions-as-sessions.pdf
 *
 * This kernel proves local binary protocol fidelity and bounded queue safety.
 * It does not claim global progress or deadlock freedom.
 * Cycle 5.3 exposes verifier-backed incremental enabledness; an optimized
 * machine must remain differentially equivalent to this reference semantics.
 */

#include "kernel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t module_id;
    uint64_t channel_id;
} ConcurrencyChannelId;

typedef enum {
    CONCURRENCY_SESSION_SEND,
    CONCURRENCY_SESSION_RECEIVE,
    CONCURRENCY_SESSION_END,
} ConcurrencySessionActionKind;

typedef struct {
    ConcurrencySessionActionKind kind;
    uint64_t payload_type_id;
} ConcurrencySessionAction;

bool concurrency_session_action_valid(ConcurrencySessionAction action);
bool concurrency_session_actions_dual(
    ConcurrencySessionAction left, ConcurrencySessionAction right);
bool concurrency_session_action_matches(
    ConcurrencySessionAction action,
    ConcurrencySessionActionKind expected, uint64_t payload_type_id);

typedef struct {
    uint64_t endpoint_id;
    ConcurrencyTaskId owner;
    QttQuantity quantity;
    const ConcurrencySessionAction *protocol;
    size_t protocol_count;
} ConcurrencyChannelEndpoint;

/* Linear authority for an endpoint transported as a higher-order payload. */
typedef struct {
    uint64_t endpoint_id;
    uint64_t protocol_type_id;
    QttPlace place;
    ConcurrencyTaskId owner;
    QttQuantity quantity;
} ConcurrencyEndpointAuthority;

typedef enum {
    CONCURRENCY_CHANNEL_STEP_SEND,
    CONCURRENCY_CHANNEL_STEP_RECEIVE,
    CONCURRENCY_CHANNEL_STEP_RENDEZVOUS,
    CONCURRENCY_CHANNEL_STEP_CLOSE,
    CONCURRENCY_CHANNEL_STEP_CANCEL,
} ConcurrencyChannelStepKind;

typedef struct {
    ConcurrencyChannelStepKind kind;
    ConcurrencyTaskId actor;
    uint64_t endpoint_id;
    ConcurrencyTaskId peer_actor;
    uint64_t peer_endpoint_id;
    uint64_t payload_type_id;
    uint64_t delegated_endpoint_id;
} ConcurrencyChannelStep;

/* A grade-one handoff taking effect before one local trace step. */
typedef struct {
    size_t step_index;
    uint64_t endpoint_id;
    ConcurrencyTaskId from;
    ConcurrencyTaskId to;
    QttQuantity quantity;
} ConcurrencyChannelOwnershipTransfer;

typedef struct {
    ConcurrencyChannelId channel_id;
    ConcurrencyChannelEndpoint left;
    ConcurrencyChannelEndpoint right;
    size_t capacity;
    const ConcurrencyEndpointAuthority *endpoint_authorities;
    size_t endpoint_authority_count;
    const ConcurrencyChannelStep *steps;
    size_t step_count;
    const ConcurrencyChannelOwnershipTransfer *ownership_transfers;
    size_t ownership_transfer_count;
} ConcurrencyChannelTrace;

typedef enum {
    CONCURRENCY_CHANNEL_VALID,
    CONCURRENCY_CHANNEL_MALFORMED,
    CONCURRENCY_CHANNEL_OUT_OF_MEMORY,
    CONCURRENCY_CHANNEL_NOT_DUAL,
    CONCURRENCY_CHANNEL_NON_LINEAR_ENDPOINT,
    CONCURRENCY_CHANNEL_WRONG_OWNER,
    CONCURRENCY_CHANNEL_UNKNOWN_ENDPOINT,
    CONCURRENCY_CHANNEL_PROTOCOL_MISMATCH,
    CONCURRENCY_CHANNEL_FULL,
    CONCURRENCY_CHANNEL_EMPTY,
    CONCURRENCY_CHANNEL_RENDEZVOUS_REQUIRED,
    CONCURRENCY_CHANNEL_PEER_CANCELLED,
    CONCURRENCY_CHANNEL_CLOSED,
    CONCURRENCY_CHANNEL_INCOMPLETE,
    CONCURRENCY_CHANNEL_DUPLICATE_AUTHORITY,
    CONCURRENCY_CHANNEL_DELEGATED_NOT_OWNED,
    CONCURRENCY_CHANNEL_DELEGATED_TYPE_MISMATCH,
} ConcurrencyChannelError;

typedef struct {
    ConcurrencyChannelError error;
    size_t step_index;
    size_t delegated_transfer_count;
    size_t delegated_cancel_count;
    size_t delegated_task_owned_count;
    size_t delegated_in_flight_count;
} ConcurrencyChannelVerification;

typedef struct ConcurrencyChannelMachine ConcurrencyChannelMachine;

typedef enum {
    CONCURRENCY_CHANNEL_BLOCK_NONE,
    CONCURRENCY_CHANNEL_BLOCK_QUEUE_FULL,
    CONCURRENCY_CHANNEL_BLOCK_QUEUE_EMPTY,
    CONCURRENCY_CHANNEL_BLOCK_AWAIT_PEER,
    CONCURRENCY_CHANNEL_BLOCK_PROTOCOL,
    CONCURRENCY_CHANNEL_BLOCK_TERMINAL,
    CONCURRENCY_CHANNEL_BLOCK_INVALID,
    CONCURRENCY_CHANNEL_BLOCK_OUT_OF_MEMORY,
} ConcurrencyChannelBlockReason;

typedef struct {
    bool enabled;
    ConcurrencyChannelBlockReason blocked;
    ConcurrencyChannelError error;
} ConcurrencyChannelMachineResult;

ConcurrencyChannelStep concurrency_channel_send(
    ConcurrencyTaskId actor, uint64_t endpoint_id, uint64_t payload_type_id);
ConcurrencyChannelStep concurrency_channel_receive(
    ConcurrencyTaskId actor, uint64_t endpoint_id, uint64_t payload_type_id);
ConcurrencyChannelStep concurrency_channel_send_endpoint(
    ConcurrencyTaskId actor, uint64_t endpoint_id,
    uint64_t delegated_endpoint_id);
ConcurrencyChannelStep concurrency_channel_receive_endpoint(
    ConcurrencyTaskId actor, uint64_t endpoint_id,
    uint64_t delegated_endpoint_id);
ConcurrencyChannelStep concurrency_channel_rendezvous(
    ConcurrencyTaskId sender, uint64_t send_endpoint_id,
    ConcurrencyTaskId receiver, uint64_t receive_endpoint_id,
    uint64_t payload_type_id);
ConcurrencyChannelStep concurrency_channel_rendezvous_endpoint(
    ConcurrencyTaskId sender, uint64_t send_endpoint_id,
    ConcurrencyTaskId receiver, uint64_t receive_endpoint_id,
    uint64_t delegated_endpoint_id);
ConcurrencyChannelStep concurrency_channel_close(
    ConcurrencyTaskId actor, uint64_t endpoint_id);
ConcurrencyChannelStep concurrency_channel_cancel(
    ConcurrencyTaskId actor, uint64_t endpoint_id);

ConcurrencyChannelVerification concurrency_channel_verify(
    const ConcurrencyChannelTrace *trace);

ConcurrencyChannelMachine *concurrency_channel_machine_new(
    const ConcurrencyChannelTrace *definition);
ConcurrencyChannelMachineResult concurrency_channel_machine_enabled(
    const ConcurrencyChannelMachine *machine, ConcurrencyChannelStep candidate);
ConcurrencyChannelMachineResult concurrency_channel_machine_step(
    ConcurrencyChannelMachine *machine, ConcurrencyChannelStep candidate);
ConcurrencyChannelMachineResult concurrency_channel_machine_transfer_owner(
    ConcurrencyChannelMachine *machine, uint64_t endpoint_id,
    ConcurrencyTaskId from, ConcurrencyTaskId to, QttQuantity quantity);
size_t concurrency_channel_machine_step_count(
    const ConcurrencyChannelMachine *machine);
bool concurrency_channel_machine_complete(
    const ConcurrencyChannelMachine *machine);
void concurrency_channel_machine_free(ConcurrencyChannelMachine *machine);

#endif
