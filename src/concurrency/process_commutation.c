#include "process_commutation.h"

#include <stdint.h>
#include <stdlib.h>

static uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static bool process_equal(ConcurrencyProcessId left, ConcurrencyProcessId right) {
    return left.module_id == right.module_id &&
           left.process_id == right.process_id;
}

static bool event_equal(
    ConcurrencyDeterministicEvent left,
    ConcurrencyDeterministicEvent right) {
    return left.kind == right.kind &&
           left.projection_index == right.projection_index &&
           left.tick == right.tick;
}

static uint64_t certificate_fingerprint(
    const ConcurrencyProcessCommutationCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->prefix_count);
    hash = mix(hash, certificate->prefix_fingerprint);
    hash = mix(hash, certificate->frontier_fingerprint);
    hash = mix(hash, certificate->left_process.module_id);
    hash = mix(hash, certificate->left_process.process_id);
    hash = mix(hash, certificate->right_process.module_id);
    hash = mix(hash, certificate->right_process.process_id);
    hash = mix(hash, certificate->left_local_index);
    hash = mix(hash, certificate->right_local_index);
    hash = mix(hash, certificate->left_event.kind);
    hash = mix(hash, certificate->left_event.projection_index);
    hash = mix(hash, certificate->left_event.tick);
    hash = mix(hash, certificate->right_event.kind);
    hash = mix(hash, certificate->right_event.projection_index);
    hash = mix(hash, certificate->right_event.tick);
    hash = mix(hash, certificate->dependence.dependent);
    hash = mix(hash, certificate->dependence.reasons);
    return mix(hash, certificate->binary.fingerprint);
}

static const ConcurrencyProcess *find_process(
    const ConcurrencyProcessProgram *program, ConcurrencyProcessId id) {
    for (size_t i = 0; i < program->process_count; i++)
        if (process_equal(program->processes[i].id, id))
            return &program->processes[i];
    return NULL;
}

static const ConcurrencyProcessFrontierEntry *find_entry(
    const ConcurrencyProcessFrontier *frontier, ConcurrencyProcessId id) {
    for (size_t i = 0; i < frontier->process_count; i++)
        if (process_equal(frontier->entries[i].process, id))
            return &frontier->entries[i];
    return NULL;
}

static ConcurrencyProcessCommutationStatus lower_prefix(
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *prefix,
    ConcurrencyDeterministicEvent *events) {
    for (size_t i = 0; i < prefix->choice_count; i++) {
        const ConcurrencyProcessChoice *choice = &prefix->choices[i];
        const ConcurrencyProcess *process = find_process(program, choice->process);
        if (!process || choice->local_index >= process->event_count)
            return CONCURRENCY_PROCESS_COMMUTATION_INVALID_PROGRAM;
        ConcurrencyProcessEvent event = process->events[choice->local_index];
        events[i] = (ConcurrencyDeterministicEvent){
            .kind = event.kind,
            .projection_index = event.projection_index,
            .tick = choice->tick,
        };
    }
    return CONCURRENCY_PROCESS_COMMUTATION_CERTIFIED;
}

static ConcurrencyProcessCommutationStatus map_commutation(
    ConcurrencyCommutationStatus status) {
    switch (status) {
    case CONCURRENCY_COMMUTATION_CERTIFIED:
        return CONCURRENCY_PROCESS_COMMUTATION_CERTIFIED;
    case CONCURRENCY_COMMUTATION_DEPENDENT:
        return CONCURRENCY_PROCESS_COMMUTATION_DEPENDENT;
    case CONCURRENCY_COMMUTATION_NOT_COENABLED:
        return CONCURRENCY_PROCESS_COMMUTATION_NOT_COENABLED;
    case CONCURRENCY_COMMUTATION_DOES_NOT_COMMUTE:
        return CONCURRENCY_PROCESS_COMMUTATION_DOES_NOT_COMMUTE;
    case CONCURRENCY_COMMUTATION_OUT_OF_MEMORY:
        return CONCURRENCY_PROCESS_COMMUTATION_OUT_OF_MEMORY;
    default:
        return CONCURRENCY_PROCESS_COMMUTATION_INVALID_PROGRAM;
    }
}

ConcurrencyProcessCommutationStatus concurrency_process_commute(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *prefix,
    ConcurrencyProcessId left, ConcurrencyProcessId right,
    ConcurrencyProcessCommutationCertificate *certificate) {
    if (!oracle || !program || !prefix || !certificate)
        return CONCURRENCY_PROCESS_COMMUTATION_INVALID_ARGUMENT;
    *certificate = (ConcurrencyProcessCommutationCertificate){0};
    ConcurrencyProcessFrontier frontier = {0};
    ConcurrencyProcessStatus frontier_status = concurrency_process_frontier_build(
        oracle, program, prefix, &frontier);
    if (frontier_status == CONCURRENCY_PROCESS_OUT_OF_MEMORY)
        return CONCURRENCY_PROCESS_COMMUTATION_OUT_OF_MEMORY;
    if (frontier_status != CONCURRENCY_PROCESS_CERTIFIED)
        return CONCURRENCY_PROCESS_COMMUTATION_INVALID_PROGRAM;
    if (process_equal(left, right)) {
        concurrency_process_frontier_free(&frontier);
        return CONCURRENCY_PROCESS_COMMUTATION_SAME_PROCESS;
    }
    const ConcurrencyProcessFrontierEntry *left_entry = find_entry(
        &frontier, left);
    const ConcurrencyProcessFrontierEntry *right_entry = find_entry(
        &frontier, right);
    if (!left_entry || !right_entry || !left_entry->enabled ||
        !right_entry->enabled) {
        concurrency_process_frontier_free(&frontier);
        return CONCURRENCY_PROCESS_COMMUTATION_NOT_COENABLED;
    }
    if (oracle->task_trace->step_count >
            SIZE_MAX - oracle->network_trace->step_count) {
        concurrency_process_frontier_free(&frontier);
        return CONCURRENCY_PROCESS_COMMUTATION_INVALID_PROGRAM;
    }
    size_t total = oracle->task_trace->step_count +
                   oracle->network_trace->step_count;
    if (!total || total > SIZE_MAX / sizeof(ConcurrencyDeterministicEvent)) {
        concurrency_process_frontier_free(&frontier);
        return CONCURRENCY_PROCESS_COMMUTATION_INVALID_PROGRAM;
    }
    ConcurrencyDeterministicEvent *events = calloc(total, sizeof(*events));
    if (!events) {
        concurrency_process_frontier_free(&frontier);
        return CONCURRENCY_PROCESS_COMMUTATION_OUT_OF_MEMORY;
    }
    ConcurrencyProcessCommutationStatus result = lower_prefix(
        program, prefix, events);
    if (result != CONCURRENCY_PROCESS_COMMUTATION_CERTIFIED) {
        free(events);
        concurrency_process_frontier_free(&frontier);
        return result;
    }
    ConcurrencyDeterministicTrace lowered = *oracle;
    lowered.events = events;
    lowered.event_count = total;
    uint64_t tick = prefix->choice_count
        ? prefix->choices[prefix->choice_count - 1].tick : 0;
    ConcurrencyDeterministicEvent left_event = {
        .kind = left_entry->next_kind,
        .projection_index = left_entry->next_projection_index,
        .tick = tick,
    };
    ConcurrencyDeterministicEvent right_event = {
        .kind = right_entry->next_kind,
        .projection_index = right_entry->next_projection_index,
        .tick = tick,
    };
    ConcurrencyDependence dependence = {0};
    if (concurrency_deterministic_dependence(
            &lowered, left_event, right_event, &dependence) !=
        CONCURRENCY_DETERMINISTIC_CERTIFIED) {
        free(events);
        concurrency_process_frontier_free(&frontier);
        return CONCURRENCY_PROCESS_COMMUTATION_INVALID_PROGRAM;
    }
    if (dependence.dependent) {
        free(events);
        concurrency_process_frontier_free(&frontier);
        return CONCURRENCY_PROCESS_COMMUTATION_DEPENDENT;
    }
    ConcurrencyCommutationCertificate binary = {0};
    ConcurrencyCommutationStatus binary_status = concurrency_deterministic_commute(
        &lowered, prefix->choice_count, &binary);
    result = map_commutation(binary_status);
    if (result == CONCURRENCY_PROCESS_COMMUTATION_CERTIFIED) {
        *certificate = (ConcurrencyProcessCommutationCertificate){
            .prefix_count = prefix->choice_count,
            .prefix_fingerprint = frontier.prefix_fingerprint,
            .frontier_fingerprint = frontier.fingerprint,
            .left_process = left,
            .right_process = right,
            .left_local_index = left_entry->next_local_index,
            .right_local_index = right_entry->next_local_index,
            .left_event = left_event,
            .right_event = right_event,
            .dependence = dependence,
            .binary = binary,
        };
        certificate->fingerprint = certificate_fingerprint(certificate);
    }
    free(events);
    concurrency_process_frontier_free(&frontier);
    return result;
}

static bool binary_equal(
    const ConcurrencyCommutationCertificate *left,
    const ConcurrencyCommutationCertificate *right) {
    return left->prefix_count == right->prefix_count &&
           event_equal(left->task_event, right->task_event) &&
           event_equal(left->channel_event, right->channel_event) &&
           left->forward_state_fingerprint == right->forward_state_fingerprint &&
           left->reverse_state_fingerprint == right->reverse_state_fingerprint &&
           left->fingerprint == right->fingerprint;
}

ConcurrencyProcessCommutationStatus concurrency_process_commutation_verify(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *prefix,
    const ConcurrencyProcessCommutationCertificate *certificate) {
    if (!certificate) return CONCURRENCY_PROCESS_COMMUTATION_INVALID_ARGUMENT;
    if (certificate->fingerprint != certificate_fingerprint(certificate))
        return CONCURRENCY_PROCESS_COMMUTATION_INVALID_CERTIFICATE;
    ConcurrencyProcessCommutationCertificate expected = {0};
    ConcurrencyProcessCommutationStatus status = concurrency_process_commute(
        oracle, program, prefix, certificate->left_process,
        certificate->right_process, &expected);
    if (status != CONCURRENCY_PROCESS_COMMUTATION_CERTIFIED)
        return CONCURRENCY_PROCESS_COMMUTATION_INVALID_CERTIFICATE;
    bool equal = certificate->prefix_count == expected.prefix_count &&
        certificate->prefix_fingerprint == expected.prefix_fingerprint &&
        certificate->frontier_fingerprint == expected.frontier_fingerprint &&
        certificate->left_local_index == expected.left_local_index &&
        certificate->right_local_index == expected.right_local_index &&
        event_equal(certificate->left_event, expected.left_event) &&
        event_equal(certificate->right_event, expected.right_event) &&
        certificate->dependence.dependent == expected.dependence.dependent &&
        certificate->dependence.reasons == expected.dependence.reasons &&
        binary_equal(&certificate->binary, &expected.binary) &&
        certificate->fingerprint == expected.fingerprint;
    return equal ? CONCURRENCY_PROCESS_COMMUTATION_CERTIFIED
                 : CONCURRENCY_PROCESS_COMMUTATION_INVALID_CERTIFICATE;
}
