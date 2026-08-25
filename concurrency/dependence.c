#include "dependence.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    ConcurrencyTaskId actors[2];
    size_t actor_count;
    ConcurrencyTaskId lifetime_tasks[2];
    size_t lifetime_count;
    ConcurrencyTaskId cleanup_task;
    bool has_cleanup_task;
    ConcurrencyChannelId channel;
    bool has_channel;
    uint64_t endpoints[3];
    size_t endpoint_count;
    const QttPlace *places;
    size_t place_count;
    uint64_t effect_scope;
    uint64_t effect_type;
    bool has_effect;
} EventFootprint;

static bool task_valid(ConcurrencyTaskId task) {
    return task.module_id != 0 && task.task_id != 0;
}

static bool task_equal(ConcurrencyTaskId left, ConcurrencyTaskId right) {
    return left.module_id == right.module_id && left.task_id == right.task_id;
}

static bool channel_equal(
    ConcurrencyChannelId left, ConcurrencyChannelId right) {
    return left.module_id == right.module_id &&
           left.channel_id == right.channel_id;
}

static void add_task(
    ConcurrencyTaskId *tasks, size_t *count, size_t capacity,
    ConcurrencyTaskId task) {
    if (!task_valid(task)) return;
    for (size_t i = 0; i < *count; i++)
        if (task_equal(tasks[i], task)) return;
    if (*count < capacity) tasks[(*count)++] = task;
}

static void add_endpoint(EventFootprint *footprint, uint64_t endpoint) {
    if (!endpoint) return;
    for (size_t i = 0; i < footprint->endpoint_count; i++)
        if (footprint->endpoints[i] == endpoint) return;
    if (footprint->endpoint_count < 3)
        footprint->endpoints[footprint->endpoint_count++] = endpoint;
}

static bool cleanup_step(ConcurrencyStepKind kind) {
    return kind == CONCURRENCY_STEP_CHECKPOINT ||
           kind == CONCURRENCY_STEP_FAIL ||
           kind == CONCURRENCY_STEP_CLEANUP_FAIL ||
           kind == CONCURRENCY_STEP_CLEANUP_COMPLETE;
}

static ConcurrencyDeterministicStatus footprint_for(
    const ConcurrencyDeterministicTrace *trace,
    ConcurrencyDeterministicEvent event, EventFootprint *footprint) {
    *footprint = (EventFootprint){0};
    if (event.kind == CONCURRENCY_DETERMINISTIC_TASK_EVENT) {
        if (!trace->task_trace || !trace->task_trace->steps ||
            event.projection_index >= trace->task_trace->step_count)
            return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;
        const ConcurrencyStep *step =
            &trace->task_trace->steps[event.projection_index];
        ConcurrencyTaskId actor =
            step->kind == CONCURRENCY_STEP_SPAWN ||
            step->kind == CONCURRENCY_STEP_JOIN ||
            step->kind == CONCURRENCY_STEP_CANCEL
                ? step->parent : step->child;
        add_task(footprint->actors, &footprint->actor_count, 2, actor);
        add_task(footprint->lifetime_tasks, &footprint->lifetime_count,
                 2, step->parent);
        add_task(footprint->lifetime_tasks, &footprint->lifetime_count,
                 2, step->child);
        if (cleanup_step(step->kind)) {
            footprint->cleanup_task = step->child;
            footprint->has_cleanup_task = true;
        }
        footprint->places = step->captures;
        footprint->place_count = step->capture_count;
        return CONCURRENCY_DETERMINISTIC_CERTIFIED;
    }
    if (event.kind != CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT ||
        !trace->network_trace || !trace->network_trace->steps ||
        event.projection_index >= trace->network_trace->step_count)
        return CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION;
    const ConcurrencyNetworkTraceStep *network_step =
        &trace->network_trace->steps[event.projection_index];
    const ConcurrencyChannelStep *step = &network_step->channel_step;
    footprint->channel = network_step->channel_id;
    footprint->has_channel = true;
    if (trace->network_trace->channels) {
        for (size_t i = 0; i < trace->network_trace->channel_count; i++) {
            const ConcurrencyNetworkChannel *channel =
                &trace->network_trace->channels[i];
            if (!channel_equal(channel->channel_id, network_step->channel_id))
                continue;
            if (channel->scope_capability_id && channel->channel_type_id) {
                footprint->effect_scope = channel->scope_capability_id;
                footprint->effect_type = channel->channel_type_id;
                footprint->has_effect = true;
            }
            break;
        }
    }
    add_task(footprint->actors, &footprint->actor_count, 2, step->actor);
    if (step->kind == CONCURRENCY_CHANNEL_STEP_RENDEZVOUS)
        add_task(footprint->actors, &footprint->actor_count, 2,
                 step->peer_actor);
    add_endpoint(footprint, step->endpoint_id);
    add_endpoint(footprint, step->peer_endpoint_id);
    add_endpoint(footprint, step->delegated_endpoint_id);
    if (step->kind == CONCURRENCY_CHANNEL_STEP_CANCEL) {
        footprint->cleanup_task = step->actor;
        footprint->has_cleanup_task = true;
    }
    return CONCURRENCY_DETERMINISTIC_CERTIFIED;
}

static bool task_sets_overlap(
    const ConcurrencyTaskId *left, size_t left_count,
    const ConcurrencyTaskId *right, size_t right_count) {
    for (size_t i = 0; i < left_count; i++)
        for (size_t j = 0; j < right_count; j++)
            if (task_equal(left[i], right[j])) return true;
    return false;
}

static const QttPlace *endpoint_place(
    const ConcurrencyNetworkTrace *trace, uint64_t endpoint) {
    if (!trace || !trace->authorities) return NULL;
    for (size_t i = 0; i < trace->authority_count; i++)
        if (trace->authorities[i].authority.endpoint.endpoint_id == endpoint)
            return &trace->authorities[i].authority.place;
    return NULL;
}

static bool authorities_overlap(
    const ConcurrencyNetworkTrace *trace,
    const EventFootprint *left, const EventFootprint *right) {
    for (size_t i = 0; i < left->endpoint_count; i++) {
        const QttPlace *left_place = endpoint_place(
            trace, left->endpoints[i]);
        for (size_t j = 0; j < right->endpoint_count; j++) {
            if (left->endpoints[i] == right->endpoints[j]) return true;
            const QttPlace *right_place = endpoint_place(
                trace, right->endpoints[j]);
            if (left_place && right_place &&
                qtt_place_overlaps(*left_place, *right_place))
                return true;
        }
        if (left_place)
            for (size_t j = 0; j < right->place_count; j++)
                if (qtt_place_overlaps(*left_place, right->places[j]))
                    return true;
    }
    for (size_t i = 0; i < right->endpoint_count; i++) {
        const QttPlace *right_place = endpoint_place(
            trace, right->endpoints[i]);
        if (right_place)
            for (size_t j = 0; j < left->place_count; j++)
                if (qtt_place_overlaps(*right_place, left->places[j]))
                    return true;
    }
    for (size_t i = 0; i < left->place_count; i++)
        for (size_t j = 0; j < right->place_count; j++)
            if (qtt_place_overlaps(left->places[i], right->places[j]))
                return true;
    return false;
}

ConcurrencyDeterministicStatus concurrency_deterministic_dependence(
    const ConcurrencyDeterministicTrace *trace,
    ConcurrencyDeterministicEvent left_event,
    ConcurrencyDeterministicEvent right_event,
    ConcurrencyDependence *result) {
    if (!trace || !result)
        return CONCURRENCY_DETERMINISTIC_INVALID_ARGUMENT;
    EventFootprint left = {0};
    EventFootprint right = {0};
    ConcurrencyDeterministicStatus status = footprint_for(
        trace, left_event, &left);
    if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED) return status;
    status = footprint_for(trace, right_event, &right);
    if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED) return status;

    uint32_t reasons = CONCURRENCY_DEPENDENCE_NONE;
    if (task_sets_overlap(left.actors, left.actor_count,
                          right.actors, right.actor_count))
        reasons |= CONCURRENCY_DEPENDENCE_TASK_ORDER;
    if (task_sets_overlap(left.lifetime_tasks, left.lifetime_count,
                          right.actors, right.actor_count) ||
        task_sets_overlap(right.lifetime_tasks, right.lifetime_count,
                          left.actors, left.actor_count))
        reasons |= CONCURRENCY_DEPENDENCE_TASK_LIFETIME;
    if (left.has_channel && right.has_channel &&
        channel_equal(left.channel, right.channel))
        reasons |= CONCURRENCY_DEPENDENCE_CHANNEL;
    if (authorities_overlap(trace->network_trace, &left, &right))
        reasons |= CONCURRENCY_DEPENDENCE_AUTHORITY;
    if (left.has_cleanup_task && right.has_cleanup_task &&
        task_equal(left.cleanup_task, right.cleanup_task))
        reasons |= CONCURRENCY_DEPENDENCE_CLEANUP;
    if (left.has_effect && right.has_effect &&
        left.effect_scope == right.effect_scope &&
        left.effect_type == right.effect_type)
        reasons |= CONCURRENCY_DEPENDENCE_EFFECT;

    *result = (ConcurrencyDependence){
        .dependent = reasons != CONCURRENCY_DEPENDENCE_NONE,
        .reasons = reasons,
    };
    return CONCURRENCY_DETERMINISTIC_CERTIFIED;
}
