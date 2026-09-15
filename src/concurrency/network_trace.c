#include "network_trace.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const ConcurrencyNetworkChannel *definition;
    size_t left_cursor;
    size_t right_cursor;
    size_t left_queued;
    size_t right_queued;
} NetworkChannelState;

typedef struct {
    const ConcurrencyNetworkInitialAuthority *definition;
    ConcurrencyTaskId owner;
    bool in_flight;
} NetworkAuthorityState;

static bool task_equal(ConcurrencyTaskId left, ConcurrencyTaskId right) {
    return left.module_id == right.module_id && left.task_id == right.task_id;
}

static bool task_valid(ConcurrencyTaskId task) {
    return task.module_id != 0 && task.task_id != 0;
}

static bool channel_equal(
    ConcurrencyChannelId left, ConcurrencyChannelId right) {
    return left.module_id == right.module_id &&
           left.channel_id == right.channel_id;
}

static bool channel_valid(ConcurrencyChannelId channel) {
    return channel.module_id != 0 && channel.channel_id != 0;
}

static bool endpoint_equal(
    ConcurrencyNetworkEndpointId left, ConcurrencyNetworkEndpointId right) {
    return left.module_id == right.module_id &&
           left.endpoint_id == right.endpoint_id;
}

static ConcurrencyNetworkEndpointId endpoint_id(
    uint64_t module_id, uint64_t endpoint) {
    return (ConcurrencyNetworkEndpointId){
        .module_id = module_id,
        .endpoint_id = endpoint,
    };
}

static size_t find_channel(
    NetworkChannelState *channels, size_t count,
    ConcurrencyChannelId channel) {
    for (size_t i = 0; i < count; i++)
        if (channel_equal(channels[i].definition->channel_id, channel))
            return i;
    return count;
}

static size_t find_authority(
    NetworkAuthorityState *authorities, size_t count,
    ConcurrencyNetworkEndpointId endpoint) {
    for (size_t i = 0; i < count; i++)
        if (authorities[i].definition->authority.endpoint.module_id ==
                endpoint.module_id &&
            authorities[i].definition->authority.endpoint.endpoint_id ==
                endpoint.endpoint_id)
            return i;
    return count;
}

static size_t find_queue_head(
    const ConcurrencyNetworkQueueEdge *edges, size_t count,
    ConcurrencyNetworkEndpointId container) {
    for (size_t i = 0; i < count; i++)
        if (endpoint_equal(edges[i].container, container) &&
            edges[i].slot == 0)
            return i;
    return count;
}

static void remove_queue_head(
    ConcurrencyNetworkQueueEdge *edges, size_t *count, size_t head,
    ConcurrencyNetworkEndpointId container) {
    for (size_t i = head + 1; i < *count; i++)
        edges[i - 1] = edges[i];
    (*count)--;
    for (size_t i = 0; i < *count; i++)
        if (endpoint_equal(edges[i].container, container))
            edges[i].slot--;
}

static bool protocols_dual(const ConcurrencyNetworkChannel *channel) {
    if (!channel_valid(channel->channel_id) ||
        !channel->left.endpoint_id || !channel->right.endpoint_id ||
        channel->left.endpoint_id == channel->right.endpoint_id ||
        !channel->left.protocol || !channel->right.protocol ||
        !channel->left.protocol_count ||
        channel->left.protocol_count != channel->right.protocol_count)
        return false;
    for (size_t i = 0; i < channel->left.protocol_count; i++)
        if (!concurrency_session_actions_dual(
                channel->left.protocol[i], channel->right.protocol[i]))
            return false;
    return channel->left.protocol[channel->left.protocol_count - 1].kind ==
               CONCURRENCY_SESSION_END &&
           channel->right.protocol[channel->right.protocol_count - 1].kind ==
               CONCURRENCY_SESSION_END;
}

ConcurrencyNetworkTraceStep concurrency_network_step(
    ConcurrencyChannelId channel_id, ConcurrencyChannelStep channel_step) {
    return (ConcurrencyNetworkTraceStep){
        .channel_id = channel_id,
        .channel_step = channel_step,
    };
}

static void free_snapshot(ConcurrencyNetworkSnapshot *snapshot) {
    if (!snapshot) return;
    free((void *)snapshot->authorities);
    free((void *)snapshot->queue_edges);
    free((void *)snapshot->cancelled_roots);
    memset(snapshot, 0, sizeof(*snapshot));
}

void concurrency_network_derived_free(ConcurrencyNetworkDerived *derived) {
    if (!derived) return;
    concurrency_network_free(&derived->certificate);
    free_snapshot(&derived->snapshot);
}

ConcurrencyNetworkTraceStatus concurrency_network_derive(
    const ConcurrencyNetworkTrace *trace, ConcurrencyNetworkDerived *derived) {
    if (!trace || !derived || !trace->channels || !trace->channel_count ||
        !trace->authorities || !trace->authority_count ||
        !trace->steps || !trace->step_count || !trace->effect_solver ||
        !trace->effects)
        return CONCURRENCY_NETWORK_TRACE_INVALID_ARGUMENT;
    memset(derived, 0, sizeof(*derived));
    NetworkChannelState *channels =
        calloc(trace->channel_count, sizeof(*channels));
    NetworkAuthorityState *authorities =
        calloc(trace->authority_count, sizeof(*authorities));
    ConcurrencyNetworkQueueEdge *edges =
        calloc(trace->step_count, sizeof(*edges));
    ConcurrencyNetworkEndpointId *roots =
        calloc(trace->step_count, sizeof(*roots));
    ConcurrencyNetworkAuthority *snapshot_authorities =
        calloc(trace->authority_count, sizeof(*snapshot_authorities));
    if (!channels || !authorities || !edges || !roots ||
        !snapshot_authorities) {
        free(channels); free(authorities); free(edges); free(roots);
        free(snapshot_authorities);
        return CONCURRENCY_NETWORK_TRACE_OUT_OF_MEMORY;
    }

    ConcurrencyNetworkTraceStatus status =
        CONCURRENCY_NETWORK_TRACE_CERTIFIED;
    for (size_t i = 0; i < trace->channel_count; i++) {
        if (!protocols_dual(&trace->channels[i])) {
            status = CONCURRENCY_NETWORK_TRACE_PROTOCOL_ERROR;
            goto fail;
        }
        if (!trace->channels[i].scope_capability_id ||
            !trace->channels[i].channel_type_id) {
            status = CONCURRENCY_NETWORK_TRACE_MALFORMED;
            goto fail;
        }
        for (size_t j = 0; j < i; j++)
            if (channel_equal(trace->channels[j].channel_id,
                              trace->channels[i].channel_id)) {
                status = CONCURRENCY_NETWORK_TRACE_MALFORMED;
                goto fail;
            }
        channels[i].definition = &trace->channels[i];
    }
    for (size_t i = 0; i < trace->authority_count; i++) {
        if (!task_valid(trace->authorities[i].owner)) {
            status = CONCURRENCY_NETWORK_TRACE_MALFORMED;
            goto fail;
        }
        for (size_t j = 0; j < i; j++)
            if (trace->authorities[j].authority.endpoint.module_id ==
                    trace->authorities[i].authority.endpoint.module_id &&
                trace->authorities[j].authority.endpoint.endpoint_id ==
                    trace->authorities[i].authority.endpoint.endpoint_id) {
                status = CONCURRENCY_NETWORK_TRACE_MALFORMED;
                goto fail;
            }
        authorities[i].definition = &trace->authorities[i];
        authorities[i].owner = trace->authorities[i].owner;
        snapshot_authorities[i] = trace->authorities[i].authority;
    }
    for (size_t i = 0; i < trace->channel_count; i++) {
        const ConcurrencyNetworkChannel *channel = &trace->channels[i];
        const ConcurrencyChannelEndpoint *ends[] = {
            &channel->left, &channel->right,
        };
        for (size_t end = 0; end < 2; end++) {
            ConcurrencyNetworkEndpointId id = endpoint_id(
                channel->channel_id.module_id, ends[end]->endpoint_id);
            size_t authority = find_authority(
                authorities, trace->authority_count, id);
            if (authority == trace->authority_count ||
                !task_equal(authorities[authority].owner, ends[end]->owner) ||
                !qtt_quantity_equal(
                    ends[end]->quantity,
                    authorities[authority].definition->authority.quantity)) {
                status = CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR;
                goto fail;
            }
        }
    }
    /* Let the existing Cycle 4.3 validator establish QTT disjointness and
     * exact grades after the trace has derived its edge/root snapshot. */

    size_t edge_count = 0;
    size_t root_count = 0;
    bool cancellation_frontier = false;
    for (size_t i = 0; i < trace->step_count; i++) {
        ConcurrencyNetworkTraceStep event = trace->steps[i];
        size_t channel_index = find_channel(
            channels, trace->channel_count, event.channel_id);
        if (channel_index == trace->channel_count) {
            status = CONCURRENCY_NETWORK_TRACE_UNKNOWN_CHANNEL;
            goto fail;
        }
        NetworkChannelState *channel = &channels[channel_index];
        ConcurrencyChannelStep step = event.channel_step;
        uint64_t module_id = event.channel_id.module_id;
        bool on_left = step.endpoint_id ==
            channel->definition->left.endpoint_id;
        bool on_right = step.endpoint_id ==
            channel->definition->right.endpoint_id;
        if (!on_left && !on_right) {
            status = CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR;
            goto fail;
        }
        ConcurrencyNetworkEndpointId carrier_id = endpoint_id(
            module_id, step.endpoint_id);
        size_t carrier_index = find_authority(
            authorities, trace->authority_count, carrier_id);
        if (carrier_index == trace->authority_count ||
            authorities[carrier_index].in_flight ||
            !task_equal(authorities[carrier_index].owner, step.actor)) {
            status = CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR;
            goto fail;
        }

        if (step.kind == CONCURRENCY_CHANNEL_STEP_CANCEL) {
            cancellation_frontier = true;
            for (size_t root = 0; root < root_count; root++)
                if (roots[root].module_id == carrier_id.module_id &&
                    roots[root].endpoint_id == carrier_id.endpoint_id) {
                    status = CONCURRENCY_NETWORK_TRACE_MALFORMED;
                    goto fail;
                }
            roots[root_count++] = carrier_id;
            continue;
        }
        if (cancellation_frontier) {
            status = CONCURRENCY_NETWORK_TRACE_MALFORMED;
            goto fail;
        }
        if (step.kind == CONCURRENCY_CHANNEL_STEP_RECEIVE) {
            if (!step.delegated_endpoint_id || !channel->definition->capacity) {
                status = step.delegated_endpoint_id
                    ? CONCURRENCY_NETWORK_TRACE_CAPACITY_ERROR
                    : CONCURRENCY_NETWORK_TRACE_UNSUPPORTED_STEP;
                goto fail;
            }
            ConcurrencyNetworkEndpointId inbound_container = endpoint_id(
                module_id, on_left
                    ? channel->definition->right.endpoint_id
                    : channel->definition->left.endpoint_id);
            size_t head = find_queue_head(edges, edge_count, inbound_container);
            if (head == edge_count ||
                edges[head].contained.endpoint_id !=
                    step.delegated_endpoint_id) {
                status = CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR;
                goto fail;
            }
            size_t delegated_index = find_authority(
                authorities, trace->authority_count, edges[head].contained);
            const ConcurrencyChannelEndpoint *endpoint = on_left
                ? &channel->definition->left : &channel->definition->right;
            size_t *cursor = on_left ? &channel->left_cursor
                                     : &channel->right_cursor;
            if (delegated_index == trace->authority_count ||
                !authorities[delegated_index].in_flight) {
                status = CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR;
                goto fail;
            }
            uint64_t delegated_type = authorities[delegated_index]
                .definition->authority.protocol_type_id;
            if (*cursor >= endpoint->protocol_count ||
                !concurrency_session_action_matches(
                    endpoint->protocol[*cursor], CONCURRENCY_SESSION_RECEIVE,
                    delegated_type)) {
                status = CONCURRENCY_NETWORK_TRACE_PROTOCOL_ERROR;
                goto fail;
            }
            size_t *inbound_queued = on_left ? &channel->right_queued
                                             : &channel->left_queued;
            authorities[delegated_index].owner = step.actor;
            authorities[delegated_index].in_flight = false;
            (*cursor)++;
            (*inbound_queued)--;
            remove_queue_head(
                edges, &edge_count, head, inbound_container);
            continue;
        }
        if (step.kind == CONCURRENCY_CHANNEL_STEP_RENDEZVOUS) {
            if (!step.delegated_endpoint_id || channel->definition->capacity) {
                status = step.delegated_endpoint_id
                    ? CONCURRENCY_NETWORK_TRACE_CAPACITY_ERROR
                    : CONCURRENCY_NETWORK_TRACE_UNSUPPORTED_STEP;
                goto fail;
            }
            bool peer_matches = on_left
                ? step.peer_endpoint_id == channel->definition->right.endpoint_id
                : step.peer_endpoint_id == channel->definition->left.endpoint_id;
            ConcurrencyNetworkEndpointId peer_id = endpoint_id(
                module_id, step.peer_endpoint_id);
            size_t peer_index = find_authority(
                authorities, trace->authority_count, peer_id);
            if (!peer_matches || peer_index == trace->authority_count ||
                authorities[peer_index].in_flight ||
                !task_equal(authorities[peer_index].owner, step.peer_actor)) {
                status = CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR;
                goto fail;
            }
            ConcurrencyNetworkEndpointId delegated_id = endpoint_id(
                module_id, step.delegated_endpoint_id);
            size_t delegated_index = find_authority(
                authorities, trace->authority_count, delegated_id);
            if (delegated_index == trace->authority_count ||
                authorities[delegated_index].in_flight ||
                !task_equal(authorities[delegated_index].owner, step.actor) ||
                delegated_index == carrier_index ||
                delegated_index == peer_index) {
                status = CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR;
                goto fail;
            }
            const ConcurrencyChannelEndpoint *sender = on_left
                ? &channel->definition->left : &channel->definition->right;
            const ConcurrencyChannelEndpoint *receiver = on_left
                ? &channel->definition->right : &channel->definition->left;
            size_t *send_cursor = on_left ? &channel->left_cursor
                                          : &channel->right_cursor;
            size_t *receive_cursor = on_left ? &channel->right_cursor
                                             : &channel->left_cursor;
            uint64_t delegated_type = authorities[delegated_index]
                .definition->authority.protocol_type_id;
            if (*send_cursor >= sender->protocol_count ||
                *receive_cursor >= receiver->protocol_count ||
                !concurrency_session_action_matches(
                    sender->protocol[*send_cursor], CONCURRENCY_SESSION_SEND,
                    delegated_type) ||
                !concurrency_session_action_matches(
                    receiver->protocol[*receive_cursor],
                    CONCURRENCY_SESSION_RECEIVE, delegated_type)) {
                status = CONCURRENCY_NETWORK_TRACE_PROTOCOL_ERROR;
                goto fail;
            }
            authorities[delegated_index].owner = step.peer_actor;
            (*send_cursor)++;
            (*receive_cursor)++;
            continue;
        }
        if (step.kind != CONCURRENCY_CHANNEL_STEP_SEND ||
            !step.delegated_endpoint_id) {
            status = CONCURRENCY_NETWORK_TRACE_UNSUPPORTED_STEP;
            goto fail;
        }
        ConcurrencyNetworkEndpointId delegated_id = endpoint_id(
            module_id, step.delegated_endpoint_id);
        size_t delegated_index = find_authority(
            authorities, trace->authority_count, delegated_id);
        if (delegated_index == trace->authority_count ||
            authorities[delegated_index].in_flight ||
            !task_equal(authorities[delegated_index].owner, step.actor) ||
            delegated_index == carrier_index) {
            status = CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR;
            goto fail;
        }
        size_t *cursor = on_left ? &channel->left_cursor
                                 : &channel->right_cursor;
        size_t *queued = on_left ? &channel->left_queued
                                 : &channel->right_queued;
        const ConcurrencyChannelEndpoint *endpoint = on_left
            ? &channel->definition->left : &channel->definition->right;
        uint64_t delegated_type = authorities[delegated_index]
            .definition->authority.protocol_type_id;
        if (*cursor >= endpoint->protocol_count ||
            !concurrency_session_action_matches(
                endpoint->protocol[*cursor], CONCURRENCY_SESSION_SEND,
                delegated_type)) {
            status = CONCURRENCY_NETWORK_TRACE_PROTOCOL_ERROR;
            goto fail;
        }
        if (!channel->definition->capacity ||
            *queued >= channel->definition->capacity) {
            status = CONCURRENCY_NETWORK_TRACE_CAPACITY_ERROR;
            goto fail;
        }
        edges[edge_count++] = (ConcurrencyNetworkQueueEdge){
            .container = carrier_id,
            .contained = delegated_id,
            .slot = *queued,
        };
        (*queued)++;
        (*cursor)++;
        authorities[delegated_index].in_flight = true;
    }
    if (!root_count) {
        status = CONCURRENCY_NETWORK_TRACE_MALFORMED;
        goto fail;
    }
    for (size_t i = 0; i < trace->step_count; i++) {
        size_t channel_index = find_channel(
            channels, trace->channel_count, trace->steps[i].channel_id);
        const ConcurrencyNetworkChannel *channel =
            channels[channel_index].definition;
        if (concurrency_authorize_channel_step(
                trace->steps[i].channel_step.kind,
                trace->effect_solver, trace->effects,
                channel->scope_capability_id, channel->channel_type_id) !=
            CONCURRENCY_CHANNEL_EFFECTS_VALID) {
            status = CONCURRENCY_NETWORK_TRACE_EFFECT_ERROR;
            goto fail;
        }
    }

    derived->snapshot = (ConcurrencyNetworkSnapshot){
        .authorities = snapshot_authorities,
        .authority_count = trace->authority_count,
        .queue_edges = edges,
        .queue_edge_count = edge_count,
        .cancelled_roots = roots,
        .cancelled_root_count = root_count,
    };
    if (concurrency_network_build(
            &derived->snapshot, &derived->certificate) !=
        CONCURRENCY_NETWORK_CERTIFIED) {
        status = CONCURRENCY_NETWORK_TRACE_CERTIFICATE_ERROR;
        free_snapshot(&derived->snapshot);
        goto fail_without_snapshot;
    }
    free(channels);
    free(authorities);
    return CONCURRENCY_NETWORK_TRACE_CERTIFIED;

fail:
    free(snapshot_authorities);
    free(edges);
    free(roots);
fail_without_snapshot:
    free(channels);
    free(authorities);
    return status;
}
