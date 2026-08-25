#include "commutation.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t mix_task(uint64_t hash, ConcurrencyTaskId task) {
    hash = mix(hash, task.module_id);
    return mix(hash, task.task_id);
}

static uint64_t mix_place(uint64_t hash, QttPlace place) {
    hash = mix(hash, place.root.module_id);
    hash = mix(hash, place.root.binder_id);
    hash = mix(hash, place.projection_id);
    hash = mix(hash, place.projection_depth);
    for (uint8_t i = 0; i < place.projection_depth; i++)
        hash = mix(hash, place.projection_path[i]);
    return hash;
}

static uint64_t semantic_state_fingerprint(
    const ConcurrencyDeterministicTrace *trace,
    size_t task_count, size_t channel_count) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, task_count);
    hash = mix(hash, channel_count);
    for (size_t i = 0; i < task_count; i++) {
        const ConcurrencyStep *step = &trace->task_trace->steps[i];
        hash = mix(hash, step->kind);
        hash = mix_task(hash, step->parent);
        hash = mix_task(hash, step->child);
        hash = mix(hash, step->capture_count);
        for (size_t j = 0; j < step->capture_count; j++)
            hash = mix_place(hash, step->captures[j]);
        hash = mix(hash, step->loan_capture_count);
        for (size_t j = 0; j < step->loan_capture_count; j++) {
            hash = mix(hash, step->loan_captures[j].module_id);
            hash = mix(hash, step->loan_captures[j].loan_id);
        }
    }
    for (size_t i = 0; i < channel_count; i++) {
        const ConcurrencyNetworkTraceStep *event =
            &trace->network_trace->steps[i];
        const ConcurrencyChannelStep *step = &event->channel_step;
        hash = mix(hash, event->channel_id.module_id);
        hash = mix(hash, event->channel_id.channel_id);
        hash = mix(hash, step->kind);
        hash = mix_task(hash, step->actor);
        hash = mix(hash, step->endpoint_id);
        hash = mix_task(hash, step->peer_actor);
        hash = mix(hash, step->peer_endpoint_id);
        hash = mix(hash, step->payload_type_id);
        hash = mix(hash, step->delegated_endpoint_id);
    }
    return hash;
}

static uint64_t certificate_fingerprint(
    const ConcurrencyCommutationCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->prefix_count);
    hash = mix(hash, certificate->task_event.kind);
    hash = mix(hash, certificate->task_event.projection_index);
    hash = mix(hash, certificate->task_event.tick);
    hash = mix(hash, certificate->channel_event.kind);
    hash = mix(hash, certificate->channel_event.projection_index);
    hash = mix(hash, certificate->channel_event.tick);
    hash = mix(hash, certificate->forward_state_fingerprint);
    return mix(hash, certificate->reverse_state_fingerprint);
}

static ConcurrencyCommutationStatus map_status(
    ConcurrencyDeterministicStatus status) {
    return status == CONCURRENCY_DETERMINISTIC_INVALID_ARGUMENT
        ? CONCURRENCY_COMMUTATION_INVALID_ARGUMENT
        : CONCURRENCY_COMMUTATION_INVALID_PROJECTION;
}

ConcurrencyCommutationStatus concurrency_deterministic_commute(
    const ConcurrencyDeterministicTrace *trace, size_t prefix_count,
    ConcurrencyCommutationCertificate *certificate) {
    if (!trace || !certificate || !trace->events || !trace->task_trace ||
        !trace->network_trace || prefix_count > trace->event_count ||
        prefix_count > SIZE_MAX - 2)
        return CONCURRENCY_COMMUTATION_INVALID_ARGUMENT;
    memset(certificate, 0, sizeof(*certificate));

    ConcurrencyDeterministicEnabled initial = {0};
    ConcurrencyDeterministicStatus status = concurrency_deterministic_enabled(
        trace, prefix_count, &initial);
    if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED)
        return map_status(status);
    if (initial.next_task_index >= trace->task_trace->step_count ||
        initial.next_channel_index >= trace->network_trace->step_count)
        return CONCURRENCY_COMMUTATION_NOT_COENABLED;

    ConcurrencyDeterministicEvent task_event =
        concurrency_deterministic_task_event(initial.next_task_index, 0);
    ConcurrencyDeterministicEvent channel_event =
        concurrency_deterministic_channel_event(initial.next_channel_index, 0);
    ConcurrencyDependence dependence = {0};
    status = concurrency_deterministic_dependence(
        trace, task_event, channel_event, &dependence);
    if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED)
        return map_status(status);
    if (dependence.dependent) return CONCURRENCY_COMMUTATION_DEPENDENT;
    if (!initial.task_enabled || !initial.channel_enabled)
        return CONCURRENCY_COMMUTATION_NOT_COENABLED;

    size_t event_count = prefix_count + 2;
    if (event_count > SIZE_MAX / sizeof(ConcurrencyDeterministicEvent))
        return CONCURRENCY_COMMUTATION_INVALID_ARGUMENT;
    ConcurrencyDeterministicEvent *events =
        malloc(event_count * sizeof(*events));
    if (!events) return CONCURRENCY_COMMUTATION_OUT_OF_MEMORY;
    for (size_t i = 0; i < prefix_count; i++) events[i] = trace->events[i];
    uint64_t tick = prefix_count ? trace->events[prefix_count - 1].tick : 0;
    task_event.tick = tick;
    channel_event.tick = tick;

    events[prefix_count] = task_event;
    events[prefix_count + 1] = channel_event;
    ConcurrencyDeterministicTrace forward = *trace;
    forward.events = events;
    forward.event_count = event_count;
    ConcurrencyDeterministicEnabled after_task = {0};
    status = concurrency_deterministic_enabled(
        &forward, prefix_count + 1, &after_task);
    if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED) {
        free(events);
        return map_status(status);
    }
    if (!after_task.channel_enabled ||
        after_task.next_channel_index != channel_event.projection_index) {
        free(events);
        return CONCURRENCY_COMMUTATION_DOES_NOT_COMMUTE;
    }
    ConcurrencyDeterministicEnabled forward_state = {0};
    status = concurrency_deterministic_enabled(
        &forward, prefix_count + 2, &forward_state);
    if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED) {
        free(events);
        return map_status(status);
    }
    uint64_t forward_fingerprint = semantic_state_fingerprint(
        trace, forward_state.next_task_index,
        forward_state.next_channel_index);

    events[prefix_count] = channel_event;
    events[prefix_count + 1] = task_event;
    ConcurrencyDeterministicTrace reverse = *trace;
    reverse.events = events;
    reverse.event_count = event_count;
    ConcurrencyDeterministicEnabled after_channel = {0};
    status = concurrency_deterministic_enabled(
        &reverse, prefix_count + 1, &after_channel);
    if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED) {
        free(events);
        return map_status(status);
    }
    if (!after_channel.task_enabled ||
        after_channel.next_task_index != task_event.projection_index) {
        free(events);
        return CONCURRENCY_COMMUTATION_DOES_NOT_COMMUTE;
    }
    ConcurrencyDeterministicEnabled reverse_state = {0};
    status = concurrency_deterministic_enabled(
        &reverse, prefix_count + 2, &reverse_state);
    if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED) {
        free(events);
        return map_status(status);
    }
    uint64_t reverse_fingerprint = semantic_state_fingerprint(
        trace, reverse_state.next_task_index,
        reverse_state.next_channel_index);
    free(events);
    if (forward_fingerprint != reverse_fingerprint)
        return CONCURRENCY_COMMUTATION_DOES_NOT_COMMUTE;
    *certificate = (ConcurrencyCommutationCertificate){
        .prefix_count = prefix_count,
        .task_event = task_event,
        .channel_event = channel_event,
        .forward_state_fingerprint = forward_fingerprint,
        .reverse_state_fingerprint = reverse_fingerprint,
    };
    certificate->fingerprint = certificate_fingerprint(certificate);
    return CONCURRENCY_COMMUTATION_CERTIFIED;
}

static bool event_equal(
    ConcurrencyDeterministicEvent left,
    ConcurrencyDeterministicEvent right) {
    return left.kind == right.kind &&
           left.projection_index == right.projection_index &&
           left.tick == right.tick;
}

ConcurrencyCommutationStatus concurrency_deterministic_commutation_verify(
    const ConcurrencyDeterministicTrace *trace,
    const ConcurrencyCommutationCertificate *certificate) {
    if (!trace || !certificate)
        return CONCURRENCY_COMMUTATION_INVALID_ARGUMENT;
    ConcurrencyCommutationCertificate expected = {0};
    ConcurrencyCommutationStatus status = concurrency_deterministic_commute(
        trace, certificate->prefix_count, &expected);
    if (status != CONCURRENCY_COMMUTATION_CERTIFIED) return status;
    bool equal = event_equal(certificate->task_event, expected.task_event) &&
        event_equal(certificate->channel_event, expected.channel_event) &&
        certificate->forward_state_fingerprint ==
            expected.forward_state_fingerprint &&
        certificate->reverse_state_fingerprint ==
            expected.reverse_state_fingerprint &&
        certificate->fingerprint == certificate_fingerprint(certificate) &&
        certificate->fingerprint == expected.fingerprint;
    return equal ? CONCURRENCY_COMMUTATION_CERTIFIED
                 : CONCURRENCY_COMMUTATION_INVALID_CERTIFICATE;
}
