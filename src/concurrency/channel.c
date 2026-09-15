#include "channel.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    const ConcurrencyChannelEndpoint *definition;
    ConcurrencyTaskId owner;
    size_t cursor;
    bool closed;
    bool cancelled;
} EndpointState;

typedef struct {
    uint64_t payload_type_id;
    uint64_t delegated_endpoint_id;
} MessageRecord;

typedef struct {
    MessageRecord *items;
    size_t head;
    size_t count;
    size_t capacity;
} MessageQueue;

typedef enum {
    AUTHORITY_TASK_OWNED,
    AUTHORITY_IN_FLIGHT,
    AUTHORITY_CANCELLED,
} AuthorityState;

typedef struct {
    ConcurrencyEndpointAuthority definition;
    ConcurrencyTaskId owner;
    AuthorityState state;
} AuthorityRecord;

static ConcurrencyChannelVerification result(
    ConcurrencyChannelError error, size_t step_index) {
    return (ConcurrencyChannelVerification){
        .error = error,
        .step_index = step_index,
    };
}

static bool task_equal(ConcurrencyTaskId left, ConcurrencyTaskId right) {
    return left.module_id == right.module_id && left.task_id == right.task_id;
}

static bool task_valid(ConcurrencyTaskId task) {
    return task.module_id != 0 && task.task_id != 0;
}

static bool place_valid(QttPlace place) {
    if (!place.root.module_id || !place.root.binder_id ||
        place.projection_depth > QTT_PLACE_MAX_DEPTH)
        return false;
    if (!place.projection_depth) return true;
    if (!place.projection_id ||
        place.projection_id != place.projection_path[0])
        return false;
    for (uint8_t i = 0; i < place.projection_depth; i++)
        if (!place.projection_path[i]) return false;
    return true;
}

bool concurrency_session_action_valid(ConcurrencySessionAction action) {
    if (action.kind == CONCURRENCY_SESSION_END)
        return action.payload_type_id == 0;
    return (action.kind == CONCURRENCY_SESSION_SEND ||
            action.kind == CONCURRENCY_SESSION_RECEIVE) &&
           action.payload_type_id != 0;
}

bool concurrency_session_actions_dual(
    ConcurrencySessionAction left, ConcurrencySessionAction right) {
    if (!concurrency_session_action_valid(left) ||
        !concurrency_session_action_valid(right)) return false;
    if (left.kind == CONCURRENCY_SESSION_END ||
        right.kind == CONCURRENCY_SESSION_END)
        return left.kind == CONCURRENCY_SESSION_END &&
               right.kind == CONCURRENCY_SESSION_END;
    return left.payload_type_id == right.payload_type_id &&
           ((left.kind == CONCURRENCY_SESSION_SEND &&
             right.kind == CONCURRENCY_SESSION_RECEIVE) ||
            (left.kind == CONCURRENCY_SESSION_RECEIVE &&
             right.kind == CONCURRENCY_SESSION_SEND));
}

bool concurrency_session_action_matches(
    ConcurrencySessionAction action,
    ConcurrencySessionActionKind expected, uint64_t payload_type_id) {
    return concurrency_session_action_valid(action) &&
        action.kind == expected &&
        (expected == CONCURRENCY_SESSION_END ||
         action.payload_type_id == payload_type_id);
}

static ConcurrencyChannelError validate_endpoint(
    const ConcurrencyChannelEndpoint *endpoint) {
    if (!endpoint || !endpoint->endpoint_id || !task_valid(endpoint->owner) ||
        !endpoint->protocol || !endpoint->protocol_count)
        return CONCURRENCY_CHANNEL_MALFORMED;
    if (!qtt_quantity_equal(endpoint->quantity, qtt_quantity_finite(1)))
        return CONCURRENCY_CHANNEL_NON_LINEAR_ENDPOINT;
    if (endpoint->protocol[endpoint->protocol_count - 1].kind !=
        CONCURRENCY_SESSION_END)
        return CONCURRENCY_CHANNEL_MALFORMED;
    for (size_t i = 0; i < endpoint->protocol_count; i++)
        if (!concurrency_session_action_valid(endpoint->protocol[i]))
            return CONCURRENCY_CHANNEL_MALFORMED;
    return CONCURRENCY_CHANNEL_VALID;
}

static EndpointState *endpoint_for(
    EndpointState *left, EndpointState *right, uint64_t endpoint_id,
    EndpointState **peer, MessageQueue *left_to_right,
    MessageQueue *right_to_left, MessageQueue **outbound,
    MessageQueue **inbound) {
    if (endpoint_id == left->definition->endpoint_id) {
        *peer = right;
        *outbound = left_to_right;
        *inbound = right_to_left;
        return left;
    }
    if (endpoint_id == right->definition->endpoint_id) {
        *peer = left;
        *outbound = right_to_left;
        *inbound = left_to_right;
        return right;
    }
    return NULL;
}

static bool queue_push(
    MessageQueue *queue, uint64_t payload_type_id,
    uint64_t delegated_endpoint_id) {
    if (queue->count == queue->capacity) return false;
    size_t tail = (queue->head + queue->count) % queue->capacity;
    queue->items[tail] = (MessageRecord){
        .payload_type_id = payload_type_id,
        .delegated_endpoint_id = delegated_endpoint_id,
    };
    queue->count++;
    return true;
}

static bool queue_peek(MessageQueue *queue, MessageRecord *message) {
    if (!queue->count) return false;
    *message = queue->items[queue->head];
    return true;
}

static void queue_pop(MessageQueue *queue) {
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
}

static size_t find_authority(
    AuthorityRecord *authorities, size_t count, uint64_t endpoint_id) {
    for (size_t i = 0; i < count; i++)
        if (authorities[i].definition.endpoint_id == endpoint_id)
            return i;
    return count;
}

ConcurrencyChannelStep concurrency_channel_send(
    ConcurrencyTaskId actor, uint64_t endpoint_id, uint64_t payload_type_id) {
    return (ConcurrencyChannelStep){
        .kind = CONCURRENCY_CHANNEL_STEP_SEND,
        .actor = actor,
        .endpoint_id = endpoint_id,
        .payload_type_id = payload_type_id,
    };
}

ConcurrencyChannelStep concurrency_channel_receive(
    ConcurrencyTaskId actor, uint64_t endpoint_id, uint64_t payload_type_id) {
    return (ConcurrencyChannelStep){
        .kind = CONCURRENCY_CHANNEL_STEP_RECEIVE,
        .actor = actor,
        .endpoint_id = endpoint_id,
        .payload_type_id = payload_type_id,
    };
}

ConcurrencyChannelStep concurrency_channel_send_endpoint(
    ConcurrencyTaskId actor, uint64_t endpoint_id,
    uint64_t delegated_endpoint_id) {
    ConcurrencyChannelStep step = concurrency_channel_send(
        actor, endpoint_id, 0);
    step.delegated_endpoint_id = delegated_endpoint_id;
    return step;
}

ConcurrencyChannelStep concurrency_channel_receive_endpoint(
    ConcurrencyTaskId actor, uint64_t endpoint_id,
    uint64_t delegated_endpoint_id) {
    ConcurrencyChannelStep step = concurrency_channel_receive(
        actor, endpoint_id, 0);
    step.delegated_endpoint_id = delegated_endpoint_id;
    return step;
}

ConcurrencyChannelStep concurrency_channel_rendezvous(
    ConcurrencyTaskId sender, uint64_t send_endpoint_id,
    ConcurrencyTaskId receiver, uint64_t receive_endpoint_id,
    uint64_t payload_type_id) {
    return (ConcurrencyChannelStep){
        .kind = CONCURRENCY_CHANNEL_STEP_RENDEZVOUS,
        .actor = sender,
        .endpoint_id = send_endpoint_id,
        .peer_actor = receiver,
        .peer_endpoint_id = receive_endpoint_id,
        .payload_type_id = payload_type_id,
    };
}

ConcurrencyChannelStep concurrency_channel_rendezvous_endpoint(
    ConcurrencyTaskId sender, uint64_t send_endpoint_id,
    ConcurrencyTaskId receiver, uint64_t receive_endpoint_id,
    uint64_t delegated_endpoint_id) {
    ConcurrencyChannelStep step = concurrency_channel_rendezvous(
        sender, send_endpoint_id, receiver, receive_endpoint_id, 0);
    step.delegated_endpoint_id = delegated_endpoint_id;
    return step;
}

ConcurrencyChannelStep concurrency_channel_close(
    ConcurrencyTaskId actor, uint64_t endpoint_id) {
    return (ConcurrencyChannelStep){
        .kind = CONCURRENCY_CHANNEL_STEP_CLOSE,
        .actor = actor,
        .endpoint_id = endpoint_id,
    };
}

ConcurrencyChannelStep concurrency_channel_cancel(
    ConcurrencyTaskId actor, uint64_t endpoint_id) {
    return (ConcurrencyChannelStep){
        .kind = CONCURRENCY_CHANNEL_STEP_CANCEL,
        .actor = actor,
        .endpoint_id = endpoint_id,
    };
}

static ConcurrencyChannelError check_action(
    EndpointState *endpoint, EndpointState *peer,
    ConcurrencyTaskId actor, ConcurrencySessionActionKind expected,
    uint64_t payload_type_id) {
    if (!task_equal(actor, endpoint->owner))
        return CONCURRENCY_CHANNEL_WRONG_OWNER;
    if (endpoint->cancelled || peer->cancelled)
        return CONCURRENCY_CHANNEL_PEER_CANCELLED;
    if (endpoint->closed) return CONCURRENCY_CHANNEL_CLOSED;
    if (endpoint->cursor >= endpoint->definition->protocol_count)
        return CONCURRENCY_CHANNEL_PROTOCOL_MISMATCH;
    ConcurrencySessionAction action =
        endpoint->definition->protocol[endpoint->cursor];
    if (!concurrency_session_action_matches(
            action, expected, payload_type_id))
        return CONCURRENCY_CHANNEL_PROTOCOL_MISMATCH;
    return CONCURRENCY_CHANNEL_VALID;
}

ConcurrencyChannelVerification concurrency_channel_verify(
    const ConcurrencyChannelTrace *trace) {
    if (!trace || !trace->channel_id.module_id || !trace->channel_id.channel_id ||
        (trace->step_count && !trace->steps) ||
        (trace->endpoint_authority_count && !trace->endpoint_authorities) ||
        (trace->ownership_transfer_count && !trace->ownership_transfers))
        return result(CONCURRENCY_CHANNEL_MALFORMED, 0);
    ConcurrencyChannelError endpoint_error = validate_endpoint(&trace->left);
    if (endpoint_error != CONCURRENCY_CHANNEL_VALID)
        return result(endpoint_error, 0);
    endpoint_error = validate_endpoint(&trace->right);
    if (endpoint_error != CONCURRENCY_CHANNEL_VALID)
        return result(endpoint_error, 0);
    if (trace->left.endpoint_id == trace->right.endpoint_id)
        return result(CONCURRENCY_CHANNEL_MALFORMED, 0);
    if (trace->left.protocol_count != trace->right.protocol_count)
        return result(CONCURRENCY_CHANNEL_NOT_DUAL, 0);
    for (size_t i = 0; i < trace->left.protocol_count; i++)
        if (!concurrency_session_actions_dual(
                trace->left.protocol[i], trace->right.protocol[i]))
            return result(CONCURRENCY_CHANNEL_NOT_DUAL, 0);
    for (size_t i = 0; i < trace->endpoint_authority_count; i++) {
        ConcurrencyEndpointAuthority authority =
            trace->endpoint_authorities[i];
        if (!authority.endpoint_id || !authority.protocol_type_id ||
            !task_valid(authority.owner) || !place_valid(authority.place))
            return result(CONCURRENCY_CHANNEL_MALFORMED, 0);
        if (!qtt_quantity_equal(
                authority.quantity, qtt_quantity_finite(1)))
            return result(CONCURRENCY_CHANNEL_NON_LINEAR_ENDPOINT, 0);
        if (authority.endpoint_id == trace->left.endpoint_id ||
            authority.endpoint_id == trace->right.endpoint_id)
            return result(CONCURRENCY_CHANNEL_DUPLICATE_AUTHORITY, 0);
        for (size_t j = 0; j < i; j++)
            if (trace->endpoint_authorities[j].endpoint_id ==
                    authority.endpoint_id ||
                qtt_place_overlaps(
                    trace->endpoint_authorities[j].place,
                    authority.place))
                return result(CONCURRENCY_CHANNEL_DUPLICATE_AUTHORITY, 0);
    }
    if (trace->capacity > SIZE_MAX / (2 * sizeof(MessageRecord)))
        return result(CONCURRENCY_CHANNEL_MALFORMED, 0);

    MessageRecord *storage = trace->capacity
        ? calloc(trace->capacity * 2, sizeof(MessageRecord)) : NULL;
    if (trace->capacity && !storage)
        return result(CONCURRENCY_CHANNEL_OUT_OF_MEMORY, 0);
    AuthorityRecord *authorities = trace->endpoint_authority_count
        ? calloc(trace->endpoint_authority_count, sizeof(AuthorityRecord))
        : NULL;
    if (trace->endpoint_authority_count && !authorities) {
        free(storage);
        return result(CONCURRENCY_CHANNEL_OUT_OF_MEMORY, 0);
    }
    for (size_t i = 0; i < trace->endpoint_authority_count; i++) {
        authorities[i].definition = trace->endpoint_authorities[i];
        authorities[i].owner = trace->endpoint_authorities[i].owner;
        authorities[i].state = AUTHORITY_TASK_OWNED;
    }
    MessageQueue left_to_right = {
        .items = storage, .capacity = trace->capacity,
    };
    MessageQueue right_to_left = {
        .items = storage ? storage + trace->capacity : NULL,
        .capacity = trace->capacity,
    };
    EndpointState left = {
        .definition = &trace->left, .owner = trace->left.owner,
    };
    EndpointState right = {
        .definition = &trace->right, .owner = trace->right.owner,
    };
    size_t delegated_transfers = 0;
    size_t delegated_cancellations = 0;

    for (size_t i = 0; i <= trace->step_count; i++) {
        for (size_t transfer_index = 0;
             transfer_index < trace->ownership_transfer_count;
             transfer_index++) {
            ConcurrencyChannelOwnershipTransfer transfer =
                trace->ownership_transfers[transfer_index];
            if (transfer.step_index > trace->step_count ||
                (transfer_index && transfer.step_index <
                    trace->ownership_transfers[transfer_index - 1].step_index)) {
                free(storage);
                free(authorities);
                return result(CONCURRENCY_CHANNEL_MALFORMED, i);
            }
            if (transfer.step_index != i) continue;
            EndpointState *transferred =
                transfer.endpoint_id == left.definition->endpoint_id ? &left :
                transfer.endpoint_id == right.definition->endpoint_id ? &right :
                NULL;
            if (!transferred) {
                free(storage);
                free(authorities);
                return result(CONCURRENCY_CHANNEL_UNKNOWN_ENDPOINT, i);
            }
            if (!qtt_quantity_equal(
                    transfer.quantity, qtt_quantity_finite(1))) {
                free(storage);
                free(authorities);
                return result(CONCURRENCY_CHANNEL_NON_LINEAR_ENDPOINT, i);
            }
            if (!task_valid(transfer.from) || !task_valid(transfer.to) ||
                task_equal(transfer.from, transfer.to)) {
                free(storage);
                free(authorities);
                return result(CONCURRENCY_CHANNEL_MALFORMED, i);
            }
            if (!task_equal(transferred->owner, transfer.from)) {
                free(storage);
                free(authorities);
                return result(CONCURRENCY_CHANNEL_WRONG_OWNER, i);
            }
            transferred->owner = transfer.to;
        }
        if (i == trace->step_count) break;
        ConcurrencyChannelStep step = trace->steps[i];
        EndpointState *peer = NULL;
        MessageQueue *outbound = NULL;
        MessageQueue *inbound = NULL;
        EndpointState *endpoint = endpoint_for(
            &left, &right, step.endpoint_id, &peer,
            &left_to_right, &right_to_left, &outbound, &inbound);
        if (!endpoint) {
            free(storage);
            free(authorities);
            return result(CONCURRENCY_CHANNEL_UNKNOWN_ENDPOINT, i);
        }
        ConcurrencyChannelError error = CONCURRENCY_CHANNEL_VALID;
        switch (step.kind) {
        case CONCURRENCY_CHANNEL_STEP_SEND: {
            if (!task_equal(step.actor, endpoint->owner)) {
                error = CONCURRENCY_CHANNEL_WRONG_OWNER;
                break;
            }
            AuthorityRecord *delegated = NULL;
            if (step.delegated_endpoint_id) {
                size_t index = find_authority(
                    authorities, trace->endpoint_authority_count,
                    step.delegated_endpoint_id);
                if (index == trace->endpoint_authority_count) {
                    error = CONCURRENCY_CHANNEL_UNKNOWN_ENDPOINT;
                    break;
                }
                delegated = &authorities[index];
                if (delegated->state != AUTHORITY_TASK_OWNED ||
                    !task_equal(delegated->owner, step.actor)) {
                    error = CONCURRENCY_CHANNEL_DELEGATED_NOT_OWNED;
                    break;
                }
                step.payload_type_id =
                    delegated->definition.protocol_type_id;
                if (endpoint->cursor >=
                        endpoint->definition->protocol_count ||
                    endpoint->definition->protocol[endpoint->cursor]
                            .payload_type_id != step.payload_type_id) {
                    error = CONCURRENCY_CHANNEL_DELEGATED_TYPE_MISMATCH;
                    break;
                }
            }
            error = check_action(endpoint, peer, step.actor,
                CONCURRENCY_SESSION_SEND, step.payload_type_id);
            if (error == CONCURRENCY_CHANNEL_VALID && !trace->capacity)
                error = CONCURRENCY_CHANNEL_RENDEZVOUS_REQUIRED;
            if (error == CONCURRENCY_CHANNEL_VALID &&
                !queue_push(outbound, step.payload_type_id,
                            step.delegated_endpoint_id))
                error = CONCURRENCY_CHANNEL_FULL;
            if (error == CONCURRENCY_CHANNEL_VALID) {
                endpoint->cursor++;
                if (delegated) delegated->state = AUTHORITY_IN_FLIGHT;
            }
            break;
        }
        case CONCURRENCY_CHANNEL_STEP_RECEIVE: {
            if (!task_equal(step.actor, endpoint->owner)) {
                error = CONCURRENCY_CHANNEL_WRONG_OWNER;
                break;
            }
            if (endpoint->cancelled || peer->cancelled) {
                error = CONCURRENCY_CHANNEL_PEER_CANCELLED;
                break;
            }
            if (endpoint->closed) {
                error = CONCURRENCY_CHANNEL_CLOSED;
                break;
            }
            MessageRecord queued = {0};
            if (!trace->capacity) {
                error = CONCURRENCY_CHANNEL_RENDEZVOUS_REQUIRED;
                break;
            }
            if (!queue_peek(inbound, &queued)) {
                error = CONCURRENCY_CHANNEL_EMPTY;
                break;
            }
            if (step.delegated_endpoint_id) {
                if (queued.delegated_endpoint_id !=
                    step.delegated_endpoint_id) {
                    error = CONCURRENCY_CHANNEL_PROTOCOL_MISMATCH;
                    break;
                }
                step.payload_type_id = queued.payload_type_id;
            } else if (queued.delegated_endpoint_id) {
                error = CONCURRENCY_CHANNEL_DELEGATED_TYPE_MISMATCH;
                break;
            }
            error = check_action(endpoint, peer, step.actor,
                CONCURRENCY_SESSION_RECEIVE, step.payload_type_id);
            if (error == CONCURRENCY_CHANNEL_VALID &&
                queued.payload_type_id != step.payload_type_id)
                error = CONCURRENCY_CHANNEL_PROTOCOL_MISMATCH;
            if (error == CONCURRENCY_CHANNEL_VALID) {
                queue_pop(inbound);
                endpoint->cursor++;
                if (step.delegated_endpoint_id) {
                    size_t index = find_authority(
                        authorities, trace->endpoint_authority_count,
                        step.delegated_endpoint_id);
                    if (index == trace->endpoint_authority_count ||
                        authorities[index].state != AUTHORITY_IN_FLIGHT) {
                        error = CONCURRENCY_CHANNEL_DELEGATED_NOT_OWNED;
                        break;
                    }
                    authorities[index].state = AUTHORITY_TASK_OWNED;
                    authorities[index].owner = step.actor;
                    delegated_transfers++;
                }
            }
            break;
        }
        case CONCURRENCY_CHANNEL_STEP_RENDEZVOUS: {
            if (trace->capacity) {
                error = CONCURRENCY_CHANNEL_PROTOCOL_MISMATCH;
                break;
            }
            if (step.peer_endpoint_id != peer->definition->endpoint_id) {
                error = CONCURRENCY_CHANNEL_UNKNOWN_ENDPOINT;
                break;
            }
            if (!task_equal(step.actor, endpoint->owner) ||
                !task_equal(step.peer_actor, peer->owner)) {
                error = CONCURRENCY_CHANNEL_WRONG_OWNER;
                break;
            }
            AuthorityRecord *delegated = NULL;
            if (step.delegated_endpoint_id) {
                size_t index = find_authority(
                    authorities, trace->endpoint_authority_count,
                    step.delegated_endpoint_id);
                if (index == trace->endpoint_authority_count) {
                    error = CONCURRENCY_CHANNEL_UNKNOWN_ENDPOINT;
                    break;
                }
                delegated = &authorities[index];
                if (delegated->state != AUTHORITY_TASK_OWNED ||
                    !task_equal(delegated->owner, step.actor)) {
                    error = CONCURRENCY_CHANNEL_DELEGATED_NOT_OWNED;
                    break;
                }
                step.payload_type_id =
                    delegated->definition.protocol_type_id;
                if (endpoint->cursor >=
                        endpoint->definition->protocol_count ||
                    endpoint->definition->protocol[endpoint->cursor]
                            .payload_type_id != step.payload_type_id) {
                    error = CONCURRENCY_CHANNEL_DELEGATED_TYPE_MISMATCH;
                    break;
                }
            }
            error = check_action(endpoint, peer, step.actor,
                CONCURRENCY_SESSION_SEND, step.payload_type_id);
            if (error == CONCURRENCY_CHANNEL_VALID)
                error = check_action(peer, endpoint, step.peer_actor,
                    CONCURRENCY_SESSION_RECEIVE, step.payload_type_id);
            if (error == CONCURRENCY_CHANNEL_VALID) {
                endpoint->cursor++;
                peer->cursor++;
                if (delegated) {
                    delegated->owner = step.peer_actor;
                    delegated_transfers++;
                }
            }
            break;
        }
        case CONCURRENCY_CHANNEL_STEP_CLOSE:
            error = check_action(endpoint, peer, step.actor,
                CONCURRENCY_SESSION_END, 0);
            if (error == CONCURRENCY_CHANNEL_VALID) {
                endpoint->cursor++;
                endpoint->closed = true;
            }
            break;
        case CONCURRENCY_CHANNEL_STEP_CANCEL:
            if (!task_equal(step.actor, endpoint->owner))
                error = CONCURRENCY_CHANNEL_WRONG_OWNER;
            else if (endpoint->closed || endpoint->cancelled)
                error = CONCURRENCY_CHANNEL_CLOSED;
            else {
                endpoint->cancelled = true;
                peer->cancelled = true;
                MessageQueue *queues[] = {
                    &left_to_right, &right_to_left,
                };
                for (size_t q = 0; q < 2; q++) {
                    MessageRecord message;
                    while (queue_peek(queues[q], &message)) {
                        if (message.delegated_endpoint_id) {
                            size_t index = find_authority(
                                authorities,
                                trace->endpoint_authority_count,
                                message.delegated_endpoint_id);
                            if (index == trace->endpoint_authority_count ||
                                authorities[index].state !=
                                    AUTHORITY_IN_FLIGHT) {
                                error = CONCURRENCY_CHANNEL_MALFORMED;
                                break;
                            }
                            authorities[index].state = AUTHORITY_CANCELLED;
                            delegated_cancellations++;
                        }
                        queue_pop(queues[q]);
                    }
                    if (error != CONCURRENCY_CHANNEL_VALID) break;
                }
                left_to_right.count = 0;
                right_to_left.count = 0;
            }
            break;
        default:
            error = CONCURRENCY_CHANNEL_MALFORMED;
            break;
        }
        if (error != CONCURRENCY_CHANNEL_VALID) {
            free(storage);
            free(authorities);
            return result(error, i);
        }
    }

    bool terminal = (left.cancelled && right.cancelled) ||
        (left.closed && right.closed &&
         left_to_right.count == 0 && right_to_left.count == 0);
    size_t task_owned = 0;
    size_t in_flight = 0;
    for (size_t i = 0; i < trace->endpoint_authority_count; i++)
        if (authorities[i].state == AUTHORITY_TASK_OWNED)
            task_owned++;
        else if (authorities[i].state == AUTHORITY_IN_FLIGHT)
            in_flight++;
    ConcurrencyChannelVerification verification = result(
        terminal ? CONCURRENCY_CHANNEL_VALID
                 : CONCURRENCY_CHANNEL_INCOMPLETE,
        trace->step_count);
    verification.delegated_transfer_count = delegated_transfers;
    verification.delegated_cancel_count = delegated_cancellations;
    verification.delegated_task_owned_count = task_owned;
    verification.delegated_in_flight_count = in_flight;
    free(storage);
    free(authorities);
    return verification;
}
