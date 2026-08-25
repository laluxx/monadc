#include "exploration.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const ConcurrencyDeterministicTrace *source;
    ConcurrencyDeterministicTrace work;
    ConcurrencyDeterministicEvent *prefix;
    size_t total_events;
    ConcurrencyExploration *result;
} Explorer;

static uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t exploration_fingerprint(
    const ConcurrencyExploration *exploration) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, exploration->mode);
    hash = mix(hash, exploration->limits.max_schedules);
    hash = mix(hash, exploration->schedule_count);
    hash = mix(hash, exploration->branch_count);
    hash = mix(hash, exploration->commutation_count);
    hash = mix(hash, exploration->pruned_choice_count);
    hash = mix(hash, exploration->fallback_branch_count);
    for (size_t i = 0; i < exploration->schedule_count; i++) {
        hash = mix(hash, exploration->schedules[i].event_count);
        hash = mix(hash, exploration->schedules[i].certificate.fingerprint);
    }
    return hash;
}

void concurrency_deterministic_exploration_free(
    ConcurrencyExploration *exploration) {
    if (!exploration) return;
    for (size_t i = 0; i < exploration->schedule_count; i++)
        free(exploration->schedules[i].events);
    free(exploration->schedules);
    *exploration = (ConcurrencyExploration){0};
}

static ConcurrencyExplorationStatus append_schedule(Explorer *explorer) {
    ConcurrencyExploration *result = explorer->result;
    if (result->schedule_count >= result->limits.max_schedules)
        return CONCURRENCY_EXPLORATION_LIMIT_EXCEEDED;
    if (result->schedule_count >=
        SIZE_MAX / sizeof(ConcurrencyExploredSchedule))
        return CONCURRENCY_EXPLORATION_OUT_OF_MEMORY;
    size_t count = result->schedule_count + 1;
    ConcurrencyExploredSchedule *grown = realloc(
        result->schedules, count * sizeof(*grown));
    if (!grown) return CONCURRENCY_EXPLORATION_OUT_OF_MEMORY;
    result->schedules = grown;
    ConcurrencyExploredSchedule *schedule = &grown[count - 1];
    *schedule = (ConcurrencyExploredSchedule){0};
    if (explorer->total_events >
        SIZE_MAX / sizeof(ConcurrencyDeterministicEvent))
        return CONCURRENCY_EXPLORATION_OUT_OF_MEMORY;
    schedule->events = malloc(
        explorer->total_events * sizeof(*schedule->events));
    if (!schedule->events) return CONCURRENCY_EXPLORATION_OUT_OF_MEMORY;
    memcpy(schedule->events, explorer->prefix,
           explorer->total_events * sizeof(*schedule->events));
    schedule->event_count = explorer->total_events;
    ConcurrencyDeterministicTrace complete = *explorer->source;
    complete.events = schedule->events;
    complete.event_count = schedule->event_count;
    if (concurrency_deterministic_run(
            &complete, &schedule->certificate) !=
        CONCURRENCY_DETERMINISTIC_CERTIFIED) {
        free(schedule->events);
        *schedule = (ConcurrencyExploredSchedule){0};
        return CONCURRENCY_EXPLORATION_INVALID_PROJECTION;
    }
    result->schedule_count = count;
    return CONCURRENCY_EXPLORATION_CERTIFIED;
}

static ConcurrencyExplorationStatus explore_prefix(
    Explorer *explorer, size_t depth) {
    ConcurrencyDeterministicEnabled enabled = {0};
    ConcurrencyDeterministicStatus status = concurrency_deterministic_enabled(
        &explorer->work, depth, &enabled);
    if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED)
        return CONCURRENCY_EXPLORATION_INVALID_PROJECTION;
    bool task_complete = enabled.next_task_index >=
        explorer->source->task_trace->step_count;
    bool channel_complete = enabled.next_channel_index >=
        explorer->source->network_trace->step_count;
    if (task_complete && channel_complete)
        return append_schedule(explorer);
    if (!enabled.task_enabled && !enabled.channel_enabled)
        return CONCURRENCY_EXPLORATION_DEADLOCK;

    bool explore_task = enabled.task_enabled;
    bool explore_channel = enabled.channel_enabled;
    if (explorer->result->mode ==
            CONCURRENCY_EXPLORATION_CERTIFIED_SOURCE &&
        explore_task && explore_channel) {
        ConcurrencyCommutationCertificate certificate = {0};
        ConcurrencyCommutationStatus commute =
            concurrency_deterministic_commute(
                &explorer->work, depth, &certificate);
        if (commute == CONCURRENCY_COMMUTATION_CERTIFIED) {
            explorer->result->commutation_count++;
            explorer->result->pruned_choice_count++;
            explore_channel = false;
        } else if (commute == CONCURRENCY_COMMUTATION_DEPENDENT ||
                   commute == CONCURRENCY_COMMUTATION_NOT_COENABLED ||
                   commute == CONCURRENCY_COMMUTATION_DOES_NOT_COMMUTE) {
            explorer->result->fallback_branch_count++;
        } else if (commute == CONCURRENCY_COMMUTATION_OUT_OF_MEMORY) {
            return CONCURRENCY_EXPLORATION_OUT_OF_MEMORY;
        } else {
            return CONCURRENCY_EXPLORATION_INVALID_PROJECTION;
        }
    }
    if (explore_task && explore_channel)
        explorer->result->branch_count++;

    if (explore_task) {
        explorer->prefix[depth] = concurrency_deterministic_task_event(
            enabled.next_task_index, depth);
        ConcurrencyExplorationStatus explored = explore_prefix(
            explorer, depth + 1);
        if (explored != CONCURRENCY_EXPLORATION_CERTIFIED) return explored;
    }
    if (explore_channel) {
        explorer->prefix[depth] = concurrency_deterministic_channel_event(
            enabled.next_channel_index, depth);
        ConcurrencyExplorationStatus explored = explore_prefix(
            explorer, depth + 1);
        if (explored != CONCURRENCY_EXPLORATION_CERTIFIED) return explored;
    }
    return CONCURRENCY_EXPLORATION_CERTIFIED;
}

ConcurrencyExplorationStatus concurrency_deterministic_explore(
    const ConcurrencyDeterministicTrace *trace,
    ConcurrencyExplorationMode mode, ConcurrencyExplorationLimits limits,
    ConcurrencyExploration *exploration) {
    if (!trace || !exploration || !trace->task_trace ||
        !trace->network_trace || !trace->events || !trace->event_count ||
        !limits.max_schedules ||
        (mode != CONCURRENCY_EXPLORATION_EXHAUSTIVE &&
         mode != CONCURRENCY_EXPLORATION_CERTIFIED_SOURCE) ||
        trace->task_trace->step_count >
            SIZE_MAX - trace->network_trace->step_count)
        return CONCURRENCY_EXPLORATION_INVALID_ARGUMENT;
    *exploration = (ConcurrencyExploration){
        .mode = mode,
        .limits = limits,
    };
    size_t total = trace->task_trace->step_count +
                   trace->network_trace->step_count;
    if (!total || total > SIZE_MAX / sizeof(ConcurrencyDeterministicEvent))
        return CONCURRENCY_EXPLORATION_INVALID_ARGUMENT;
    ConcurrencyDeterministicEvent *prefix = calloc(total, sizeof(*prefix));
    if (!prefix) return CONCURRENCY_EXPLORATION_OUT_OF_MEMORY;
    Explorer explorer = {
        .source = trace,
        .work = *trace,
        .prefix = prefix,
        .total_events = total,
        .result = exploration,
    };
    explorer.work.events = prefix;
    explorer.work.event_count = total;
    ConcurrencyExplorationStatus status = explore_prefix(&explorer, 0);
    free(prefix);
    if (status != CONCURRENCY_EXPLORATION_CERTIFIED) {
        concurrency_deterministic_exploration_free(exploration);
        return status;
    }
    exploration->fingerprint = exploration_fingerprint(exploration);
    return CONCURRENCY_EXPLORATION_CERTIFIED;
}

static bool event_equal(
    ConcurrencyDeterministicEvent left,
    ConcurrencyDeterministicEvent right) {
    return left.kind == right.kind &&
           left.projection_index == right.projection_index &&
           left.tick == right.tick;
}

static bool deterministic_certificate_equal(
    const ConcurrencyDeterministicCertificate *left,
    const ConcurrencyDeterministicCertificate *right) {
    return left->event_count == right->event_count &&
           left->final_tick == right->final_tick &&
           left->cleanup_fingerprint == right->cleanup_fingerprint &&
           left->schedule_fingerprint == right->schedule_fingerprint &&
           left->fingerprint == right->fingerprint;
}

static bool exploration_equal(
    const ConcurrencyExploration *left,
    const ConcurrencyExploration *right) {
    if (left->mode != right->mode ||
        left->limits.max_schedules != right->limits.max_schedules ||
        left->schedule_count != right->schedule_count ||
        left->branch_count != right->branch_count ||
        left->commutation_count != right->commutation_count ||
        left->pruned_choice_count != right->pruned_choice_count ||
        left->fallback_branch_count != right->fallback_branch_count ||
        left->fingerprint != exploration_fingerprint(left) ||
        left->fingerprint != right->fingerprint)
        return false;
    for (size_t i = 0; i < left->schedule_count; i++) {
        const ConcurrencyExploredSchedule *a = &left->schedules[i];
        const ConcurrencyExploredSchedule *b = &right->schedules[i];
        if (a->event_count != b->event_count ||
            !deterministic_certificate_equal(
                &a->certificate, &b->certificate))
            return false;
        for (size_t j = 0; j < a->event_count; j++)
            if (!event_equal(a->events[j], b->events[j])) return false;
    }
    return true;
}

ConcurrencyExplorationStatus concurrency_deterministic_exploration_verify(
    const ConcurrencyDeterministicTrace *trace,
    const ConcurrencyExploration *exploration) {
    if (!trace || !exploration)
        return CONCURRENCY_EXPLORATION_INVALID_ARGUMENT;
    if ((exploration->schedule_count && !exploration->schedules) ||
        !exploration->limits.max_schedules)
        return CONCURRENCY_EXPLORATION_INVALID_CERTIFICATE;
    for (size_t i = 0; i < exploration->schedule_count; i++) {
        if (exploration->schedules[i].event_count &&
            !exploration->schedules[i].events)
            return CONCURRENCY_EXPLORATION_INVALID_CERTIFICATE;
        ConcurrencyDeterministicTrace replay = *trace;
        replay.events = exploration->schedules[i].events;
        replay.event_count = exploration->schedules[i].event_count;
        if (concurrency_deterministic_verify(
                &replay, &exploration->schedules[i].certificate) !=
            CONCURRENCY_DETERMINISTIC_CERTIFIED)
            return CONCURRENCY_EXPLORATION_INVALID_CERTIFICATE;
    }
    ConcurrencyExploration expected = {0};
    ConcurrencyExplorationStatus status = concurrency_deterministic_explore(
        trace, exploration->mode, exploration->limits, &expected);
    if (status != CONCURRENCY_EXPLORATION_CERTIFIED) return status;
    bool equal = exploration_equal(exploration, &expected);
    concurrency_deterministic_exploration_free(&expected);
    return equal ? CONCURRENCY_EXPLORATION_CERTIFIED
                 : CONCURRENCY_EXPLORATION_INVALID_CERTIFICATE;
}
