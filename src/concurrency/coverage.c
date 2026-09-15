#include "coverage.h"

#include <stdint.h>
#include <stdlib.h>

static uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static bool event_equal(
    ConcurrencyDeterministicEvent left,
    ConcurrencyDeterministicEvent right) {
    return left.kind == right.kind &&
           left.projection_index == right.projection_index &&
           left.tick == right.tick;
}

static bool schedule_equal(
    const ConcurrencyExploredSchedule *left,
    const ConcurrencyExploredSchedule *right) {
    if (left->event_count != right->event_count) return false;
    for (size_t i = 0; i < left->event_count; i++)
        if (!event_equal(left->events[i], right->events[i])) return false;
    return true;
}

static bool swap_neighbor(
    const ConcurrencyExploredSchedule *from,
    const ConcurrencyExploredSchedule *to, size_t position) {
    if (from->event_count != to->event_count ||
        position + 1 >= from->event_count)
        return false;
    for (size_t i = 0; i < from->event_count; i++) {
        size_t source = i == position ? position + 1
                      : i == position + 1 ? position : i;
        if (from->events[source].kind != to->events[i].kind ||
            from->events[source].projection_index !=
                to->events[i].projection_index)
            return false;
    }
    return true;
}

static uint64_t coverage_fingerprint(const ConcurrencyCoverage *coverage) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, coverage->limits.max_swaps_per_witness);
    hash = mix(hash, coverage->exhaustive_exploration_fingerprint);
    hash = mix(hash, coverage->reduced_exploration_fingerprint);
    hash = mix(hash, coverage->witness_count);
    hash = mix(hash, coverage->covered_schedule_count);
    hash = mix(hash, coverage->total_swap_count);
    for (size_t i = 0; i < coverage->witness_count; i++) {
        const ConcurrencyCoverageWitness *witness = &coverage->witnesses[i];
        hash = mix(hash, witness->exhaustive_schedule_index);
        hash = mix(hash, witness->representative_schedule_index);
        hash = mix(hash, witness->exhaustive_schedule_fingerprint);
        hash = mix(hash, witness->representative_schedule_fingerprint);
        hash = mix(hash, witness->swap_count);
        for (size_t j = 0; j < witness->swap_count; j++)
            hash = mix(hash, witness->swap_positions[j]);
    }
    return hash;
}

void concurrency_deterministic_coverage_free(ConcurrencyCoverage *coverage) {
    if (!coverage) return;
    for (size_t i = 0; i < coverage->witness_count; i++)
        free(coverage->witnesses[i].swap_positions);
    free(coverage->witnesses);
    *coverage = (ConcurrencyCoverage){0};
}

static size_t representative_for(
    const ConcurrencyExploration *reduced,
    const ConcurrencyExploredSchedule *schedule) {
    for (size_t i = 0; i < reduced->schedule_count; i++)
        if (schedule_equal(schedule, &reduced->schedules[i])) return i;
    return reduced->schedule_count;
}

static ConcurrencyCoverageStatus build_witness(
    const ConcurrencyDeterministicTrace *trace,
    const ConcurrencyExploration *exhaustive,
    const ConcurrencyExploration *reduced,
    size_t source_index, ConcurrencyCoverageLimits limits,
    ConcurrencyCoverageWitness *witness) {
    size_t count = exhaustive->schedule_count;
    if (!count || count > SIZE_MAX / sizeof(size_t))
        return CONCURRENCY_COVERAGE_INVALID_EXPLORATION;
    bool *visited = calloc(count, sizeof(*visited));
    size_t *parent = malloc(count * sizeof(*parent));
    size_t *parent_swap = malloc(count * sizeof(*parent_swap));
    size_t *queue = malloc(count * sizeof(*queue));
    if (!visited || !parent || !parent_swap || !queue) {
        free(visited);
        free(parent);
        free(parent_swap);
        free(queue);
        return CONCURRENCY_COVERAGE_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < count; i++) parent[i] = SIZE_MAX;
    size_t head = 0;
    size_t tail = 0;
    queue[tail++] = source_index;
    visited[source_index] = true;
    size_t target = count;
    size_t representative = reduced->schedule_count;

    while (head < tail && target == count) {
        size_t current = queue[head++];
        const ConcurrencyExploredSchedule *schedule =
            &exhaustive->schedules[current];
        representative = representative_for(reduced, schedule);
        if (representative < reduced->schedule_count) {
            target = current;
            break;
        }
        ConcurrencyDeterministicTrace replay = *trace;
        replay.events = schedule->events;
        replay.event_count = schedule->event_count;
        for (size_t position = 0;
             position + 1 < schedule->event_count; position++) {
            if (schedule->events[position].kind ==
                schedule->events[position + 1].kind)
                continue;
            ConcurrencyCommutationCertificate commute = {0};
            if (concurrency_deterministic_commute(
                    &replay, position, &commute) !=
                CONCURRENCY_COMMUTATION_CERTIFIED)
                continue;
            for (size_t next = 0; next < count; next++) {
                if (visited[next] ||
                    !swap_neighbor(schedule, &exhaustive->schedules[next],
                                   position))
                    continue;
                visited[next] = true;
                parent[next] = current;
                parent_swap[next] = position;
                queue[tail++] = next;
                break;
            }
        }
    }
    if (target == count) {
        free(visited);
        free(parent);
        free(parent_swap);
        free(queue);
        return CONCURRENCY_COVERAGE_UNCOVERED_SCHEDULE;
    }
    size_t swap_count = 0;
    for (size_t cursor = target; cursor != source_index;
         cursor = parent[cursor]) {
        if (cursor == SIZE_MAX || parent[cursor] == SIZE_MAX) {
            free(visited);
            free(parent);
            free(parent_swap);
            free(queue);
            return CONCURRENCY_COVERAGE_INVALID_EXPLORATION;
        }
        swap_count++;
    }
    if (swap_count > limits.max_swaps_per_witness) {
        free(visited);
        free(parent);
        free(parent_swap);
        free(queue);
        return CONCURRENCY_COVERAGE_LIMIT_EXCEEDED;
    }
    size_t *swaps = swap_count ? malloc(swap_count * sizeof(*swaps)) : NULL;
    if (swap_count && !swaps) {
        free(visited);
        free(parent);
        free(parent_swap);
        free(queue);
        return CONCURRENCY_COVERAGE_OUT_OF_MEMORY;
    }
    size_t cursor = target;
    for (size_t i = swap_count; i > 0; i--) {
        swaps[i - 1] = parent_swap[cursor];
        cursor = parent[cursor];
    }
    *witness = (ConcurrencyCoverageWitness){
        .exhaustive_schedule_index = source_index,
        .representative_schedule_index = representative,
        .exhaustive_schedule_fingerprint = exhaustive->schedules[source_index]
            .certificate.schedule_fingerprint,
        .representative_schedule_fingerprint = reduced->schedules[representative]
            .certificate.schedule_fingerprint,
        .swap_positions = swaps,
        .swap_count = swap_count,
    };
    free(visited);
    free(parent);
    free(parent_swap);
    free(queue);
    return CONCURRENCY_COVERAGE_CERTIFIED;
}

ConcurrencyCoverageStatus concurrency_deterministic_coverage_build(
    const ConcurrencyDeterministicTrace *trace,
    const ConcurrencyExploration *exhaustive,
    const ConcurrencyExploration *reduced,
    ConcurrencyCoverageLimits limits,
    ConcurrencyCoverage *coverage) {
    if (!trace || !exhaustive || !reduced || !coverage ||
        !limits.max_swaps_per_witness ||
        exhaustive->mode != CONCURRENCY_EXPLORATION_EXHAUSTIVE ||
        reduced->mode != CONCURRENCY_EXPLORATION_CERTIFIED_SOURCE ||
        !exhaustive->schedule_count || !reduced->schedule_count)
        return CONCURRENCY_COVERAGE_INVALID_ARGUMENT;
    *coverage = (ConcurrencyCoverage){
        .limits = limits,
        .exhaustive_exploration_fingerprint = exhaustive->fingerprint,
        .reduced_exploration_fingerprint = reduced->fingerprint,
    };
    if (concurrency_deterministic_exploration_verify(trace, exhaustive) !=
            CONCURRENCY_EXPLORATION_CERTIFIED ||
        concurrency_deterministic_exploration_verify(trace, reduced) !=
            CONCURRENCY_EXPLORATION_CERTIFIED)
        return CONCURRENCY_COVERAGE_INVALID_EXPLORATION;
    if (exhaustive->schedule_count >
        SIZE_MAX / sizeof(ConcurrencyCoverageWitness))
        return CONCURRENCY_COVERAGE_OUT_OF_MEMORY;
    coverage->witnesses = calloc(
        exhaustive->schedule_count, sizeof(*coverage->witnesses));
    if (!coverage->witnesses) return CONCURRENCY_COVERAGE_OUT_OF_MEMORY;
    coverage->witness_count = exhaustive->schedule_count;
    for (size_t i = 0; i < exhaustive->schedule_count; i++) {
        ConcurrencyCoverageStatus status = build_witness(
            trace, exhaustive, reduced, i, limits, &coverage->witnesses[i]);
        if (status != CONCURRENCY_COVERAGE_CERTIFIED) {
            concurrency_deterministic_coverage_free(coverage);
            return status;
        }
        coverage->covered_schedule_count++;
        if (coverage->total_swap_count >
            SIZE_MAX - coverage->witnesses[i].swap_count) {
            concurrency_deterministic_coverage_free(coverage);
            return CONCURRENCY_COVERAGE_OUT_OF_MEMORY;
        }
        coverage->total_swap_count += coverage->witnesses[i].swap_count;
    }
    coverage->fingerprint = coverage_fingerprint(coverage);
    return CONCURRENCY_COVERAGE_CERTIFIED;
}

static ConcurrencyCoverageStatus verify_witness(
    const ConcurrencyDeterministicTrace *trace,
    const ConcurrencyExploration *exhaustive,
    const ConcurrencyExploration *reduced,
    const ConcurrencyCoverageWitness *witness, size_t expected_source,
    size_t max_swaps) {
    if (witness->exhaustive_schedule_index != expected_source ||
        witness->representative_schedule_index >= reduced->schedule_count ||
        witness->swap_count > max_swaps ||
        (witness->swap_count && !witness->swap_positions))
        return CONCURRENCY_COVERAGE_INVALID_CERTIFICATE;
    const ConcurrencyExploredSchedule *source =
        &exhaustive->schedules[expected_source];
    const ConcurrencyExploredSchedule *representative =
        &reduced->schedules[witness->representative_schedule_index];
    if (witness->exhaustive_schedule_fingerprint !=
            source->certificate.schedule_fingerprint ||
        witness->representative_schedule_fingerprint !=
            representative->certificate.schedule_fingerprint ||
        source->event_count != representative->event_count ||
        source->event_count >
            SIZE_MAX / sizeof(ConcurrencyDeterministicEvent))
        return CONCURRENCY_COVERAGE_INVALID_CERTIFICATE;
    ConcurrencyDeterministicEvent *events = malloc(
        source->event_count * sizeof(*events));
    if (!events) return CONCURRENCY_COVERAGE_OUT_OF_MEMORY;
    for (size_t i = 0; i < source->event_count; i++)
        events[i] = source->events[i];
    ConcurrencyDeterministicTrace replay = *trace;
    replay.events = events;
    replay.event_count = source->event_count;
    for (size_t i = 0; i < witness->swap_count; i++) {
        size_t position = witness->swap_positions[i];
        if (position + 1 >= source->event_count ||
            events[position].kind == events[position + 1].kind) {
            free(events);
            return CONCURRENCY_COVERAGE_INVALID_CERTIFICATE;
        }
        ConcurrencyCommutationCertificate commute = {0};
        if (concurrency_deterministic_commute(
                &replay, position, &commute) !=
            CONCURRENCY_COMMUTATION_CERTIFIED) {
            free(events);
            return CONCURRENCY_COVERAGE_INVALID_CERTIFICATE;
        }
        uint64_t left_tick = events[position].tick;
        uint64_t right_tick = events[position + 1].tick;
        ConcurrencyDeterministicEvent temporary = events[position];
        events[position] = events[position + 1];
        events[position + 1] = temporary;
        events[position].tick = left_tick;
        events[position + 1].tick = right_tick;
    }
    bool equal = true;
    for (size_t i = 0; i < source->event_count; i++)
        if (!event_equal(events[i], representative->events[i])) {
            equal = false;
            break;
        }
    free(events);
    return equal ? CONCURRENCY_COVERAGE_CERTIFIED
                 : CONCURRENCY_COVERAGE_INVALID_CERTIFICATE;
}

ConcurrencyCoverageStatus concurrency_deterministic_coverage_verify(
    const ConcurrencyDeterministicTrace *trace,
    const ConcurrencyExploration *exhaustive,
    const ConcurrencyExploration *reduced,
    const ConcurrencyCoverage *coverage) {
    if (!trace || !exhaustive || !reduced || !coverage ||
        !coverage->limits.max_swaps_per_witness)
        return CONCURRENCY_COVERAGE_INVALID_ARGUMENT;
    if (coverage->witness_count && !coverage->witnesses)
        return CONCURRENCY_COVERAGE_INVALID_CERTIFICATE;
    for (size_t i = 0; i < coverage->witness_count; i++)
        if (coverage->witnesses[i].swap_count &&
            !coverage->witnesses[i].swap_positions)
            return CONCURRENCY_COVERAGE_INVALID_CERTIFICATE;
    if (concurrency_deterministic_exploration_verify(trace, exhaustive) !=
            CONCURRENCY_EXPLORATION_CERTIFIED ||
        concurrency_deterministic_exploration_verify(trace, reduced) !=
            CONCURRENCY_EXPLORATION_CERTIFIED)
        return CONCURRENCY_COVERAGE_INVALID_EXPLORATION;
    if (coverage->exhaustive_exploration_fingerprint !=
            exhaustive->fingerprint ||
        coverage->reduced_exploration_fingerprint != reduced->fingerprint ||
        coverage->witness_count != exhaustive->schedule_count ||
        coverage->covered_schedule_count != exhaustive->schedule_count ||
        coverage->fingerprint != coverage_fingerprint(coverage))
        return CONCURRENCY_COVERAGE_INVALID_CERTIFICATE;
    size_t total_swaps = 0;
    for (size_t i = 0; i < coverage->witness_count; i++) {
        ConcurrencyCoverageStatus status = verify_witness(
            trace, exhaustive, reduced, &coverage->witnesses[i], i,
            coverage->limits.max_swaps_per_witness);
        if (status != CONCURRENCY_COVERAGE_CERTIFIED) return status;
        if (total_swaps > SIZE_MAX - coverage->witnesses[i].swap_count)
            return CONCURRENCY_COVERAGE_INVALID_CERTIFICATE;
        total_swaps += coverage->witnesses[i].swap_count;
    }
    return total_swaps == coverage->total_swap_count
        ? CONCURRENCY_COVERAGE_CERTIFIED
        : CONCURRENCY_COVERAGE_INVALID_CERTIFICATE;
}
