#include "deterministic.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool task_equal(ConcurrencyTaskId left, ConcurrencyTaskId right) {
    return left.module_id == right.module_id && left.task_id == right.task_id;
}

static bool channel_equal(
    ConcurrencyChannelId left, ConcurrencyChannelId right) {
    return left.module_id == right.module_id &&
           left.channel_id == right.channel_id;
}

static const ConcurrencyNetworkChannel *find_channel_definition(
    const ConcurrencyNetworkTrace *trace, ConcurrencyChannelId channel) {
    for (size_t i = 0; i < trace->channel_count; i++)
        if (channel_equal(trace->channels[i].channel_id, channel))
            return &trace->channels[i];
    return NULL;
}

static uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t schedule_fingerprint(
    const ConcurrencyDeterministicTrace *trace) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, trace->event_count);
    for (size_t i = 0; i < trace->event_count; i++) {
        hash = mix(hash, trace->events[i].kind);
        hash = mix(hash, trace->events[i].projection_index);
        hash = mix(hash, trace->events[i].tick);
    }
    return hash;
}

static uint64_t certificate_fingerprint(
    const ConcurrencyDeterministicCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->event_count);
    hash = mix(hash, certificate->final_tick);
    hash = mix(hash, certificate->cleanup_fingerprint);
    hash = mix(hash, certificate->schedule_fingerprint);
    return hash;
}

ConcurrencyDeterministicEvent concurrency_deterministic_task_event(
    size_t projection_index, uint64_t tick) {
    return (ConcurrencyDeterministicEvent){
        .kind = CONCURRENCY_DETERMINISTIC_TASK_EVENT,
        .projection_index = projection_index,
        .tick = tick,
    };
}

ConcurrencyDeterministicEvent concurrency_deterministic_channel_event(
    size_t projection_index, uint64_t tick) {
    return (ConcurrencyDeterministicEvent){
        .kind = CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT,
        .projection_index = projection_index,
        .tick = tick,
    };
}

ConcurrencyDeterministicStatus concurrency_deterministic_channel_enabled(
    const ConcurrencyNetworkTrace *trace, size_t candidate_index,
    ConcurrencyChannelMachineResult *result) {
    if (!trace || !result || !trace->channels || !trace->channel_count ||
        !trace->authorities || !trace->steps ||
        candidate_index >= trace->step_count)
        return CONCURRENCY_DETERMINISTIC_INVALID_ARGUMENT;
    const ConcurrencyNetworkTraceStep *candidate =
        &trace->steps[candidate_index];
    const ConcurrencyNetworkChannel *channel = find_channel_definition(
        trace, candidate->channel_id);
    if (!channel) return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;
    if (concurrency_authorize_channel_step(
            candidate->channel_step.kind, trace->effect_solver,
            trace->effects, channel->scope_capability_id,
            channel->channel_type_id) !=
        CONCURRENCY_CHANNEL_EFFECTS_VALID)
        return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;
    if (trace->authority_count >
        SIZE_MAX / sizeof(ConcurrencyEndpointAuthority))
        return CONCURRENCY_DETERMINISTIC_INVALID_ARGUMENT;
    ConcurrencyEndpointAuthority *authorities = trace->authority_count
        ? calloc(trace->authority_count, sizeof(*authorities)) : NULL;
    if (trace->authority_count && !authorities)
        return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;
    size_t authority_count = 0;
    for (size_t i = 0; i < trace->authority_count; i++) {
        const ConcurrencyNetworkInitialAuthority *source =
            &trace->authorities[i];
        if (source->authority.endpoint.module_id !=
            channel->channel_id.module_id)
            continue;
        uint64_t endpoint = source->authority.endpoint.endpoint_id;
        if (endpoint == channel->left.endpoint_id ||
            endpoint == channel->right.endpoint_id)
            continue;
        authorities[authority_count++] = (ConcurrencyEndpointAuthority){
            .endpoint_id = endpoint,
            .protocol_type_id = source->authority.protocol_type_id,
            .place = source->authority.place,
            .owner = source->owner,
            .quantity = source->authority.quantity,
        };
    }
    ConcurrencyChannelTrace definition = {
        .channel_id = channel->channel_id,
        .left = channel->left,
        .right = channel->right,
        .capacity = channel->capacity,
        .endpoint_authorities = authorities,
        .endpoint_authority_count = authority_count,
    };
    ConcurrencyChannelMachine *machine =
        concurrency_channel_machine_new(&definition);
    if (!machine) {
        free(authorities);
        return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;
    }
    ConcurrencyTaskId carrier_owners[2] = {
        channel->left.owner, channel->right.owner,
    };
    bool carrier_in_flight[2] = {false, false};
    ConcurrencyChannelId flight_channels[2] = {0};
    uint64_t flight_receivers[2] = {0, 0};
    uint64_t carrier_ids[2] = {
        channel->left.endpoint_id, channel->right.endpoint_id,
    };
    for (size_t i = 0; i < candidate_index; i++) {
        const ConcurrencyNetworkTraceStep *event = &trace->steps[i];
        if (channel_equal(event->channel_id, channel->channel_id)) {
            ConcurrencyChannelMachineResult accepted =
                concurrency_channel_machine_step(
                    machine, event->channel_step);
            if (!accepted.enabled) {
                concurrency_channel_machine_free(machine);
                free(authorities);
                return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;
            }
        }
        const ConcurrencyChannelStep *step = &event->channel_step;
        if (!step->delegated_endpoint_id) continue;
        for (size_t carrier = 0; carrier < 2; carrier++) {
            if (step->delegated_endpoint_id != carrier_ids[carrier])
                continue;
            ConcurrencyTaskId next_owner = carrier_owners[carrier];
            bool transfer = false;
            switch (step->kind) {
            case CONCURRENCY_CHANNEL_STEP_SEND:
                if (carrier_in_flight[carrier] ||
                    !task_equal(step->actor, carrier_owners[carrier]))
                    goto invalid_projection;
                {
                    const ConcurrencyNetworkChannel *transport =
                        find_channel_definition(trace, event->channel_id);
                    if (!transport) goto invalid_projection;
                    if (step->endpoint_id == transport->left.endpoint_id)
                        flight_receivers[carrier] =
                            transport->right.endpoint_id;
                    else if (step->endpoint_id ==
                             transport->right.endpoint_id)
                        flight_receivers[carrier] =
                            transport->left.endpoint_id;
                    else
                        goto invalid_projection;
                }
                flight_channels[carrier] = event->channel_id;
                carrier_in_flight[carrier] = true;
                break;
            case CONCURRENCY_CHANNEL_STEP_RECEIVE:
                if (!carrier_in_flight[carrier] ||
                    !channel_equal(event->channel_id,
                                   flight_channels[carrier]) ||
                    step->endpoint_id != flight_receivers[carrier])
                    goto invalid_projection;
                next_owner = step->actor;
                carrier_in_flight[carrier] = false;
                transfer = true;
                break;
            case CONCURRENCY_CHANNEL_STEP_RENDEZVOUS:
                if (carrier_in_flight[carrier] ||
                    !task_equal(step->actor, carrier_owners[carrier]))
                    goto invalid_projection;
                next_owner = step->peer_actor;
                transfer = true;
                break;
            default:
                goto invalid_projection;
            }
            if (transfer) {
                ConcurrencyChannelMachineResult handed_off =
                    concurrency_channel_machine_transfer_owner(
                        machine, carrier_ids[carrier],
                        carrier_owners[carrier], next_owner,
                        qtt_quantity_finite(1));
                if (!handed_off.enabled) goto invalid_projection;
                carrier_owners[carrier] = next_owner;
            }
        }
    }
    for (size_t carrier = 0; carrier < 2; carrier++)
        if (candidate->channel_step.endpoint_id == carrier_ids[carrier] &&
            carrier_in_flight[carrier])
            goto invalid_projection;
    *result = concurrency_channel_machine_enabled(
        machine, candidate->channel_step);
    concurrency_channel_machine_free(machine);
    free(authorities);
    return CONCURRENCY_DETERMINISTIC_CERTIFIED;

invalid_projection:
            concurrency_channel_machine_free(machine);
            free(authorities);
            return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;
}

static bool task_is_observed_cleaning(
    const ConcurrencyDeterministicTrace *trace, size_t before,
    ConcurrencyTaskId target) {
    bool cancellation_pending = false;
    bool cleaning = false;
    for (size_t i = 0; i < before; i++) {
        const ConcurrencyDeterministicEvent *event = &trace->events[i];
        if (event->kind != CONCURRENCY_DETERMINISTIC_TASK_EVENT) continue;
        const ConcurrencyStep *step =
            &trace->task_trace->steps[event->projection_index];
        if (!task_equal(step->child, target)) continue;
        switch (step->kind) {
        case CONCURRENCY_STEP_CANCEL:
            cancellation_pending = true;
            break;
        case CONCURRENCY_STEP_CHECKPOINT:
            if (cancellation_pending) cleaning = true;
            break;
        case CONCURRENCY_STEP_CLEANUP_COMPLETE:
        case CONCURRENCY_STEP_JOIN:
            cleaning = false;
            break;
        default:
            break;
        }
    }
    return cleaning;
}

static bool task_is_live(
    const ConcurrencyDeterministicTrace *trace, size_t before,
    ConcurrencyTaskId target) {
    bool live = task_equal(trace->task_trace->root, target);
    for (size_t i = 0; i < before; i++) {
        const ConcurrencyDeterministicEvent *event = &trace->events[i];
        if (event->kind != CONCURRENCY_DETERMINISTIC_TASK_EVENT) continue;
        const ConcurrencyStep *step =
            &trace->task_trace->steps[event->projection_index];
        if (!task_equal(step->child, target)) continue;
        if (step->kind == CONCURRENCY_STEP_SPAWN) live = true;
        if (step->kind == CONCURRENCY_STEP_JOIN) live = false;
    }
    return live;
}

static bool event_selected(
    const ConcurrencyDeterministicEvent *selected, size_t selected_count,
    ConcurrencyDeterministicEvent candidate) {
    for (size_t i = 0; i < selected_count; i++)
        if (selected[i].kind == candidate.kind &&
            selected[i].projection_index == candidate.projection_index)
            return true;
    return false;
}

ConcurrencyDeterministicStatus concurrency_deterministic_temporal_enabled(
    const ConcurrencyTrace *task_trace,
    const ConcurrencyNetworkTrace *network_trace,
    const ConcurrencyDeterministicEvent *selected, size_t selected_count,
    ConcurrencyDeterministicEvent candidate, bool *enabled,
    ConcurrencyDeterministicBlockReason *blocked) {
    if (!task_trace || !network_trace || !enabled || !blocked ||
        (selected_count && !selected) ||
        (task_trace->step_count && !task_trace->steps) ||
        (network_trace->step_count && !network_trace->steps))
        return CONCURRENCY_DETERMINISTIC_INVALID_ARGUMENT;
    for (size_t i = 0; i < selected_count; i++) {
        if ((selected[i].kind == CONCURRENCY_DETERMINISTIC_TASK_EVENT &&
             selected[i].projection_index >= task_trace->step_count) ||
            (selected[i].kind == CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT &&
             selected[i].projection_index >= network_trace->step_count) ||
            (selected[i].kind != CONCURRENCY_DETERMINISTIC_TASK_EVENT &&
             selected[i].kind != CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT))
            return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;
    }
    ConcurrencyDeterministicTrace trace = {
        .task_trace = task_trace,
        .network_trace = network_trace,
        .events = selected,
        .event_count = selected_count,
    };
    if (candidate.kind == CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT) {
        if (candidate.projection_index >= network_trace->step_count)
            return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;
        const ConcurrencyChannelStep *step =
            &network_trace->steps[candidate.projection_index].channel_step;
        bool actor_live = task_is_live(&trace, selected_count, step->actor);
        bool peer_live = step->kind != CONCURRENCY_CHANNEL_STEP_RENDEZVOUS ||
            task_is_live(&trace, selected_count, step->peer_actor);
        bool cleaning = step->kind != CONCURRENCY_CHANNEL_STEP_CANCEL ||
            task_is_observed_cleaning(&trace, selected_count, step->actor);
        *enabled = actor_live && peer_live && cleaning;
        *blocked = !actor_live || !peer_live
            ? CONCURRENCY_DETERMINISTIC_TASK_NOT_LIVE
            : !cleaning
                ? CONCURRENCY_DETERMINISTIC_TASK_NOT_CLEANING
                : CONCURRENCY_DETERMINISTIC_ENABLED;
        return CONCURRENCY_DETERMINISTIC_CERTIFIED;
    }
    if (candidate.kind != CONCURRENCY_DETERMINISTIC_TASK_EVENT ||
        candidate.projection_index >= task_trace->step_count)
        return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;
    const ConcurrencyStep *step = &task_trace->steps[candidate.projection_index];
    bool waits = false;
    if (step->kind == CONCURRENCY_STEP_CLEANUP_COMPLETE ||
        step->kind == CONCURRENCY_STEP_JOIN) {
        for (size_t i = 0; i < network_trace->step_count; i++) {
            const ConcurrencyChannelStep *network_step =
                &network_trace->steps[i].channel_step;
            ConcurrencyDeterministicEvent event =
                concurrency_deterministic_channel_event(i, 0);
            if (network_step->kind == CONCURRENCY_CHANNEL_STEP_CANCEL &&
                task_equal(network_step->actor, step->child) &&
                !event_selected(selected, selected_count, event)) {
                waits = true;
                break;
            }
        }
    }
    *enabled = !waits;
    *blocked = waits
        ? CONCURRENCY_DETERMINISTIC_AWAITS_CLEANUP_DISCHARGE
        : CONCURRENCY_DETERMINISTIC_ENABLED;
    return CONCURRENCY_DETERMINISTIC_CERTIFIED;
}

static ConcurrencyDeterministicStatus prefix_indices(
    const ConcurrencyDeterministicTrace *trace, size_t prefix_count,
    size_t *next_task, size_t *next_channel) {
    if (prefix_count > trace->event_count)
        return CONCURRENCY_DETERMINISTIC_MALFORMED_SCHEDULE;
    *next_task = 0;
    *next_channel = 0;
    uint64_t previous_tick = 0;
    for (size_t i = 0; i < prefix_count; i++) {
        const ConcurrencyDeterministicEvent *event = &trace->events[i];
        if (i && event->tick < previous_tick)
            return CONCURRENCY_DETERMINISTIC_TIME_REGRESSION;
        previous_tick = event->tick;
        if (event->kind == CONCURRENCY_DETERMINISTIC_TASK_EVENT) {
            if (event->projection_index != *next_task ||
                *next_task >= trace->task_trace->step_count)
                return CONCURRENCY_DETERMINISTIC_MALFORMED_SCHEDULE;
            (*next_task)++;
        } else if (event->kind == CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT) {
            if (event->projection_index != *next_channel ||
                *next_channel >= trace->network_trace->step_count)
                return CONCURRENCY_DETERMINISTIC_MALFORMED_SCHEDULE;
            (*next_channel)++;
        } else {
            return CONCURRENCY_DETERMINISTIC_MALFORMED_SCHEDULE;
        }
    }
    return CONCURRENCY_DETERMINISTIC_CERTIFIED;
}

ConcurrencyDeterministicStatus concurrency_deterministic_enabled(
    const ConcurrencyDeterministicTrace *trace, size_t prefix_count,
    ConcurrencyDeterministicEnabled *enabled) {
    if (!trace || !enabled || !trace->task_trace || !trace->network_trace ||
        !trace->events || !trace->event_count)
        return CONCURRENCY_DETERMINISTIC_INVALID_ARGUMENT;
    memset(enabled, 0, sizeof(*enabled));
    ConcurrencyCleanupBridgeCertificate cleanup = {0};
    if (concurrency_cleanup_bridge_build(
            trace->network_trace, trace->task_trace, &cleanup) !=
        CONCURRENCY_CLEANUP_BRIDGE_CERTIFIED)
        return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;
    concurrency_cleanup_bridge_free(&cleanup);
    size_t next_task = 0;
    size_t next_channel = 0;
    ConcurrencyDeterministicStatus status = prefix_indices(
        trace, prefix_count, &next_task, &next_channel);
    if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED) return status;
    enabled->next_task_index = next_task;
    enabled->next_channel_index = next_channel;

    if (next_task == trace->task_trace->step_count) {
        enabled->task_blocked = CONCURRENCY_DETERMINISTIC_PROJECTION_COMPLETE;
    } else {
        status = concurrency_deterministic_temporal_enabled(
            trace->task_trace, trace->network_trace, trace->events,
            prefix_count, concurrency_deterministic_task_event(next_task, 0),
            &enabled->task_enabled, &enabled->task_blocked);
        if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED) return status;
    }

    if (next_channel == trace->network_trace->step_count) {
        enabled->channel_blocked =
            CONCURRENCY_DETERMINISTIC_PROJECTION_COMPLETE;
    } else {
        status = concurrency_deterministic_temporal_enabled(
            trace->task_trace, trace->network_trace, trace->events,
            prefix_count,
            concurrency_deterministic_channel_event(next_channel, 0),
            &enabled->channel_enabled, &enabled->channel_blocked);
        if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED) return status;
        if (enabled->channel_enabled) {
            ConcurrencyChannelMachineResult local = {0};
            status = concurrency_deterministic_channel_enabled(
                trace->network_trace, next_channel, &local);
            if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED) return status;
            enabled->local_channel_blocked = local.blocked;
            if (!local.enabled) {
                enabled->channel_enabled = false;
                enabled->channel_blocked =
                    CONCURRENCY_DETERMINISTIC_LOCAL_CHANNEL_BLOCKED;
            }
        }
    }
    return CONCURRENCY_DETERMINISTIC_CERTIFIED;
}

ConcurrencyDeterministicStatus concurrency_deterministic_run(
    const ConcurrencyDeterministicTrace *trace,
    ConcurrencyDeterministicCertificate *certificate) {
    if (!trace || !certificate || !trace->task_trace ||
        !trace->network_trace || !trace->events || !trace->event_count)
        return CONCURRENCY_DETERMINISTIC_INVALID_ARGUMENT;
    memset(certificate, 0, sizeof(*certificate));
    if (trace->task_trace->step_count > SIZE_MAX -
            trace->network_trace->step_count ||
        trace->event_count != trace->task_trace->step_count +
            trace->network_trace->step_count)
        return CONCURRENCY_DETERMINISTIC_MALFORMED_SCHEDULE;

    ConcurrencyCleanupBridgeCertificate cleanup = {0};
    if (concurrency_cleanup_bridge_build(
            trace->network_trace, trace->task_trace, &cleanup) !=
        CONCURRENCY_CLEANUP_BRIDGE_CERTIFIED)
        return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;

    size_t next_task = 0;
    size_t next_channel = 0;
    uint64_t previous_tick = 0;
    for (size_t i = 0; i < trace->event_count; i++) {
        const ConcurrencyDeterministicEvent *event = &trace->events[i];
        if (i && event->tick < previous_tick) {
            concurrency_cleanup_bridge_free(&cleanup);
            return CONCURRENCY_DETERMINISTIC_TIME_REGRESSION;
        }
        previous_tick = event->tick;
        ConcurrencyDeterministicEnabled enabled = {0};
        ConcurrencyDeterministicStatus enabled_status =
            concurrency_deterministic_enabled(trace, i, &enabled);
        if (enabled_status != CONCURRENCY_DETERMINISTIC_CERTIFIED) {
            concurrency_cleanup_bridge_free(&cleanup);
            return enabled_status;
        }
        bool selected_enabled =
            event->kind == CONCURRENCY_DETERMINISTIC_TASK_EVENT
                ? enabled.task_enabled : enabled.channel_enabled;
        if (!selected_enabled) {
            ConcurrencyDeterministicBlockReason reason =
                event->kind == CONCURRENCY_DETERMINISTIC_TASK_EVENT
                    ? enabled.task_blocked : enabled.channel_blocked;
            concurrency_cleanup_bridge_free(&cleanup);
            return reason == CONCURRENCY_DETERMINISTIC_TASK_NOT_CLEANING ||
                   reason ==
                       CONCURRENCY_DETERMINISTIC_AWAITS_CLEANUP_DISCHARGE
                ? CONCURRENCY_DETERMINISTIC_CANCEL_OUTSIDE_CLEANUP
                : CONCURRENCY_DETERMINISTIC_EVENT_BLOCKED;
        }
        if (event->kind == CONCURRENCY_DETERMINISTIC_TASK_EVENT) {
            if (event->projection_index != next_task ||
                next_task >= trace->task_trace->step_count) {
                concurrency_cleanup_bridge_free(&cleanup);
                return CONCURRENCY_DETERMINISTIC_MALFORMED_SCHEDULE;
            }
            next_task++;
            continue;
        }
        if (event->kind != CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT ||
            event->projection_index != next_channel ||
            next_channel >= trace->network_trace->step_count) {
            concurrency_cleanup_bridge_free(&cleanup);
            return CONCURRENCY_DETERMINISTIC_MALFORMED_SCHEDULE;
        }
        const ConcurrencyChannelStep *step =
            &trace->network_trace->steps[next_channel].channel_step;
        if (step->kind == CONCURRENCY_CHANNEL_STEP_CANCEL &&
            !task_is_observed_cleaning(trace, i, step->actor)) {
            concurrency_cleanup_bridge_free(&cleanup);
            return CONCURRENCY_DETERMINISTIC_CANCEL_OUTSIDE_CLEANUP;
        }
        next_channel++;
    }
    if (next_task != trace->task_trace->step_count ||
        next_channel != trace->network_trace->step_count) {
        concurrency_cleanup_bridge_free(&cleanup);
        return CONCURRENCY_DETERMINISTIC_MALFORMED_SCHEDULE;
    }
    certificate->event_count = trace->event_count;
    certificate->final_tick = previous_tick;
    certificate->cleanup_fingerprint = cleanup.fingerprint;
    certificate->schedule_fingerprint = schedule_fingerprint(trace);
    certificate->fingerprint = certificate_fingerprint(certificate);
    concurrency_cleanup_bridge_free(&cleanup);
    return CONCURRENCY_DETERMINISTIC_CERTIFIED;
}

ConcurrencyDeterministicStatus concurrency_deterministic_verify(
    const ConcurrencyDeterministicTrace *trace,
    const ConcurrencyDeterministicCertificate *certificate) {
    if (!trace || !certificate)
        return CONCURRENCY_DETERMINISTIC_INVALID_ARGUMENT;
    ConcurrencyDeterministicCertificate expected = {0};
    ConcurrencyDeterministicStatus status =
        concurrency_deterministic_run(trace, &expected);
    if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED) return status;
    bool equal = certificate->event_count == expected.event_count &&
        certificate->final_tick == expected.final_tick &&
        certificate->cleanup_fingerprint == expected.cleanup_fingerprint &&
        certificate->schedule_fingerprint == expected.schedule_fingerprint &&
        certificate->fingerprint == certificate_fingerprint(certificate) &&
        certificate->fingerprint == expected.fingerprint;
    return equal ? CONCURRENCY_DETERMINISTIC_CERTIFIED
                 : CONCURRENCY_DETERMINISTIC_INVALID_CERTIFICATE;
}
