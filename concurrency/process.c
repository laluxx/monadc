#include "process.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static bool process_equal(ConcurrencyProcessId left, ConcurrencyProcessId right) {
    return left.module_id == right.module_id &&
           left.process_id == right.process_id;
}

static uint64_t program_fingerprint(const ConcurrencyProcessProgram *program) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, program->process_count);
    for (size_t i = 0; i < program->process_count; i++) {
        const ConcurrencyProcess *process = &program->processes[i];
        hash = mix(hash, process->id.module_id);
        hash = mix(hash, process->id.process_id);
        hash = mix(hash, process->event_count);
        for (size_t j = 0; j < process->event_count; j++) {
            hash = mix(hash, process->events[j].kind);
            hash = mix(hash, process->events[j].projection_index);
        }
    }
    return hash;
}

static uint64_t schedule_fingerprint(
    const ConcurrencyProcessSchedule *schedule) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, schedule->choice_count);
    for (size_t i = 0; i < schedule->choice_count; i++) {
        hash = mix(hash, schedule->choices[i].process.module_id);
        hash = mix(hash, schedule->choices[i].process.process_id);
        hash = mix(hash, schedule->choices[i].local_index);
        hash = mix(hash, schedule->choices[i].tick);
    }
    return hash;
}

static uint64_t certificate_fingerprint(
    const ConcurrencyProcessCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->process_count);
    hash = mix(hash, certificate->event_count);
    hash = mix(hash, certificate->program_fingerprint);
    hash = mix(hash, certificate->schedule_fingerprint);
    hash = mix(hash, certificate->deterministic.fingerprint);
    return hash;
}

static uint64_t frontier_fingerprint(
    const ConcurrencyProcessFrontier *frontier) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, frontier->process_count);
    hash = mix(hash, frontier->enabled_count);
    hash = mix(hash, frontier->prefix_count);
    hash = mix(hash, frontier->program_fingerprint);
    hash = mix(hash, frontier->prefix_fingerprint);
    hash = mix(hash, frontier->binary.task_enabled);
    hash = mix(hash, frontier->binary.channel_enabled);
    hash = mix(hash, frontier->binary.task_blocked);
    hash = mix(hash, frontier->binary.channel_blocked);
    hash = mix(hash, frontier->binary.local_channel_blocked);
    hash = mix(hash, frontier->binary.next_task_index);
    hash = mix(hash, frontier->binary.next_channel_index);
    for (size_t i = 0; i < frontier->process_count; i++) {
        const ConcurrencyProcessFrontierEntry *entry = &frontier->entries[i];
        hash = mix(hash, entry->process.module_id);
        hash = mix(hash, entry->process.process_id);
        hash = mix(hash, entry->next_local_index);
        hash = mix(hash, entry->enabled);
        hash = mix(hash, entry->blocked);
        hash = mix(hash, entry->binary_reason);
        hash = mix(hash, entry->next_kind);
        hash = mix(hash, entry->next_projection_index);
    }
    return hash;
}

ConcurrencyProcessEvent concurrency_process_event(
    ConcurrencyDeterministicEventKind kind, size_t projection_index) {
    return (ConcurrencyProcessEvent){
        .kind = kind,
        .projection_index = projection_index,
    };
}

ConcurrencyProcessChoice concurrency_process_choice(
    ConcurrencyProcessId process, size_t local_index, uint64_t tick) {
    return (ConcurrencyProcessChoice){
        .process = process,
        .local_index = local_index,
        .tick = tick,
    };
}

static const ConcurrencyProcess *find_process(
    const ConcurrencyProcessProgram *program, ConcurrencyProcessId id) {
    for (size_t i = 0; i < program->process_count; i++)
        if (process_equal(program->processes[i].id, id))
            return &program->processes[i];
    return NULL;
}

static ConcurrencyProcessStatus validate_program(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program, size_t *total_out) {
    if (!oracle || !oracle->task_trace || !oracle->network_trace ||
        !program || !program->processes || !program->process_count)
        return CONCURRENCY_PROCESS_INVALID_ARGUMENT;
    size_t task_count = oracle->task_trace->step_count;
    size_t channel_count = oracle->network_trace->step_count;
    if (task_count > SIZE_MAX - channel_count)
        return CONCURRENCY_PROCESS_INVALID_ARGUMENT;
    size_t total = task_count + channel_count;
    bool *tasks = task_count ? calloc(task_count, sizeof(*tasks)) : NULL;
    bool *channels = channel_count
        ? calloc(channel_count, sizeof(*channels)) : NULL;
    if ((task_count && !tasks) || (channel_count && !channels)) {
        free(tasks);
        free(channels);
        return CONCURRENCY_PROCESS_OUT_OF_MEMORY;
    }
    size_t observed = 0;
    ConcurrencyProcessStatus status = CONCURRENCY_PROCESS_CERTIFIED;
    for (size_t i = 0; i < program->process_count; i++) {
        const ConcurrencyProcess *process = &program->processes[i];
        if (!process->id.module_id || !process->id.process_id ||
            (process->event_count && !process->events)) {
            status = CONCURRENCY_PROCESS_INVALID_PROGRAM;
            break;
        }
        for (size_t prior = 0; prior < i; prior++)
            if (process_equal(process->id, program->processes[prior].id)) {
                status = CONCURRENCY_PROCESS_INVALID_PROGRAM;
                break;
            }
        if (status != CONCURRENCY_PROCESS_CERTIFIED) break;
        if (observed > SIZE_MAX - process->event_count) {
            status = CONCURRENCY_PROCESS_INVALID_PROGRAM;
            break;
        }
        observed += process->event_count;
        for (size_t j = 0; j < process->event_count; j++) {
            ConcurrencyProcessEvent event = process->events[j];
            bool *seen = NULL;
            if (event.kind == CONCURRENCY_DETERMINISTIC_TASK_EVENT &&
                event.projection_index < task_count)
                seen = &tasks[event.projection_index];
            else if (event.kind == CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT &&
                     event.projection_index < channel_count)
                seen = &channels[event.projection_index];
            else {
                status = CONCURRENCY_PROCESS_INVALID_PROGRAM;
                break;
            }
            if (*seen) {
                status = CONCURRENCY_PROCESS_INVALID_PROGRAM;
                break;
            }
            *seen = true;
        }
        if (status != CONCURRENCY_PROCESS_CERTIFIED) break;
    }
    if (status == CONCURRENCY_PROCESS_CERTIFIED && observed != total)
        status = CONCURRENCY_PROCESS_INVALID_PROGRAM;
    free(tasks);
    free(channels);
    if (status == CONCURRENCY_PROCESS_CERTIFIED) *total_out = total;
    return status;
}

static ConcurrencyProcessStatus lower_schedule(
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *schedule, size_t total,
    ConcurrencyDeterministicEvent *events) {
    if (!schedule || (schedule->choice_count && !schedule->choices) ||
        schedule->choice_count != total)
        return CONCURRENCY_PROCESS_MALFORMED_SCHEDULE;
    if (program->process_count > SIZE_MAX / sizeof(size_t))
        return CONCURRENCY_PROCESS_OUT_OF_MEMORY;
    size_t *next = calloc(program->process_count, sizeof(*next));
    if (!next) return CONCURRENCY_PROCESS_OUT_OF_MEMORY;
    ConcurrencyProcessStatus status = CONCURRENCY_PROCESS_CERTIFIED;
    for (size_t i = 0; i < total; i++) {
        const ConcurrencyProcessChoice *choice = &schedule->choices[i];
        const ConcurrencyProcess *process = find_process(program, choice->process);
        if (!process) {
            status = CONCURRENCY_PROCESS_MALFORMED_SCHEDULE;
            break;
        }
        size_t process_index = (size_t)(process - program->processes);
        if (choice->local_index != next[process_index] ||
            choice->local_index >= process->event_count) {
            status = CONCURRENCY_PROCESS_MALFORMED_SCHEDULE;
            break;
        }
        ConcurrencyProcessEvent source = process->events[choice->local_index];
        events[i] = (ConcurrencyDeterministicEvent){
            .kind = source.kind,
            .projection_index = source.projection_index,
            .tick = choice->tick,
        };
        next[process_index]++;
    }
    for (size_t i = 0;
         status == CONCURRENCY_PROCESS_CERTIFIED && i < program->process_count;
         i++)
        if (next[i] != program->processes[i].event_count)
            status = CONCURRENCY_PROCESS_MALFORMED_SCHEDULE;
    free(next);
    return status;
}

ConcurrencyProcessStatus concurrency_process_run(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *schedule,
    ConcurrencyProcessCertificate *certificate) {
    if (!certificate) return CONCURRENCY_PROCESS_INVALID_ARGUMENT;
    *certificate = (ConcurrencyProcessCertificate){0};
    size_t total = 0;
    ConcurrencyProcessStatus status = validate_program(oracle, program, &total);
    if (status != CONCURRENCY_PROCESS_CERTIFIED) return status;
    if (!total || total > SIZE_MAX / sizeof(ConcurrencyDeterministicEvent))
        return CONCURRENCY_PROCESS_INVALID_PROGRAM;
    ConcurrencyDeterministicEvent *events = malloc(total * sizeof(*events));
    if (!events) return CONCURRENCY_PROCESS_OUT_OF_MEMORY;
    status = lower_schedule(program, schedule, total, events);
    if (status != CONCURRENCY_PROCESS_CERTIFIED) {
        free(events);
        return status;
    }
    ConcurrencyDeterministicTrace lowered = *oracle;
    lowered.events = events;
    lowered.event_count = total;
    ConcurrencyDeterministicStatus deterministic = concurrency_deterministic_run(
        &lowered, &certificate->deterministic);
    free(events);
    if (deterministic != CONCURRENCY_DETERMINISTIC_CERTIFIED) {
        *certificate = (ConcurrencyProcessCertificate){0};
        return CONCURRENCY_PROCESS_INVALID_PROJECTION;
    }
    certificate->process_count = program->process_count;
    certificate->event_count = total;
    certificate->program_fingerprint = program_fingerprint(program);
    certificate->schedule_fingerprint = schedule_fingerprint(schedule);
    certificate->fingerprint = certificate_fingerprint(certificate);
    return CONCURRENCY_PROCESS_CERTIFIED;
}

ConcurrencyProcessStatus concurrency_process_verify(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *schedule,
    const ConcurrencyProcessCertificate *certificate) {
    if (!certificate) return CONCURRENCY_PROCESS_INVALID_ARGUMENT;
    ConcurrencyProcessCertificate expected = {0};
    ConcurrencyProcessStatus status = concurrency_process_run(
        oracle, program, schedule, &expected);
    if (status != CONCURRENCY_PROCESS_CERTIFIED)
        return CONCURRENCY_PROCESS_INVALID_CERTIFICATE;
    return certificate->process_count == expected.process_count &&
           certificate->event_count == expected.event_count &&
           certificate->program_fingerprint == expected.program_fingerprint &&
           certificate->schedule_fingerprint == expected.schedule_fingerprint &&
           certificate->deterministic.event_count ==
               expected.deterministic.event_count &&
           certificate->deterministic.final_tick ==
               expected.deterministic.final_tick &&
           certificate->deterministic.cleanup_fingerprint ==
               expected.deterministic.cleanup_fingerprint &&
           certificate->deterministic.schedule_fingerprint ==
               expected.deterministic.schedule_fingerprint &&
           certificate->deterministic.fingerprint ==
               expected.deterministic.fingerprint &&
           certificate->fingerprint == expected.fingerprint &&
           certificate->fingerprint == certificate_fingerprint(certificate)
        ? CONCURRENCY_PROCESS_CERTIFIED
        : CONCURRENCY_PROCESS_INVALID_CERTIFICATE;
}

void concurrency_process_frontier_free(ConcurrencyProcessFrontier *frontier) {
    if (!frontier) return;
    free(frontier->entries);
    *frontier = (ConcurrencyProcessFrontier){0};
}

static ConcurrencyProcessStatus lower_prefix(
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *prefix, size_t total,
    ConcurrencyDeterministicEvent *events, size_t *next) {
    if (!prefix || prefix->choice_count > total ||
        (prefix->choice_count && !prefix->choices))
        return CONCURRENCY_PROCESS_MALFORMED_SCHEDULE;
    for (size_t i = 0; i < prefix->choice_count; i++) {
        const ConcurrencyProcessChoice *choice = &prefix->choices[i];
        const ConcurrencyProcess *process = find_process(program, choice->process);
        if (!process) return CONCURRENCY_PROCESS_MALFORMED_SCHEDULE;
        size_t process_index = (size_t)(process - program->processes);
        if (choice->local_index != next[process_index] ||
            choice->local_index >= process->event_count)
            return CONCURRENCY_PROCESS_MALFORMED_SCHEDULE;
        ConcurrencyProcessEvent source = process->events[choice->local_index];
        events[i] = (ConcurrencyDeterministicEvent){
            .kind = source.kind,
            .projection_index = source.projection_index,
            .tick = choice->tick,
        };
        next[process_index]++;
    }
    return CONCURRENCY_PROCESS_CERTIFIED;
}

ConcurrencyProcessStatus concurrency_process_frontier_build(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *prefix,
    ConcurrencyProcessFrontier *frontier) {
    if (!frontier) return CONCURRENCY_PROCESS_INVALID_ARGUMENT;
    *frontier = (ConcurrencyProcessFrontier){0};
    size_t total = 0;
    ConcurrencyProcessStatus status = validate_program(oracle, program, &total);
    if (status != CONCURRENCY_PROCESS_CERTIFIED) return status;
    if (!total || total > SIZE_MAX / sizeof(ConcurrencyDeterministicEvent) ||
        program->process_count > SIZE_MAX / sizeof(size_t) ||
        program->process_count >
            SIZE_MAX / sizeof(ConcurrencyProcessFrontierEntry))
        return CONCURRENCY_PROCESS_INVALID_PROGRAM;
    ConcurrencyDeterministicEvent *events = calloc(total, sizeof(*events));
    size_t *next = calloc(program->process_count, sizeof(*next));
    ConcurrencyProcessFrontierEntry *entries = calloc(
        program->process_count, sizeof(*entries));
    if (!events || !next || !entries) {
        free(events);
        free(next);
        free(entries);
        return CONCURRENCY_PROCESS_OUT_OF_MEMORY;
    }
    status = lower_prefix(program, prefix, total, events, next);
    if (status != CONCURRENCY_PROCESS_CERTIFIED) {
        free(events);
        free(next);
        free(entries);
        return status;
    }
    ConcurrencyDeterministicTrace lowered = *oracle;
    lowered.events = events;
    lowered.event_count = total;
    ConcurrencyDeterministicEnabled binary = {0};
    if (concurrency_deterministic_enabled(
            &lowered, prefix->choice_count, &binary) !=
        CONCURRENCY_DETERMINISTIC_CERTIFIED) {
        free(events);
        free(next);
        free(entries);
        return CONCURRENCY_PROCESS_INVALID_PROJECTION;
    }
    frontier->entries = entries;
    frontier->process_count = program->process_count;
    frontier->prefix_count = prefix->choice_count;
    frontier->program_fingerprint = program_fingerprint(program);
    frontier->prefix_fingerprint = schedule_fingerprint(prefix);
    frontier->binary = binary;
    for (size_t i = 0; i < program->process_count; i++) {
        const ConcurrencyProcess *process = &program->processes[i];
        ConcurrencyProcessFrontierEntry *entry = &entries[i];
        entry->process = process->id;
        entry->next_local_index = next[i];
        if (next[i] == process->event_count) {
            entry->blocked = CONCURRENCY_PROCESS_STREAM_COMPLETE;
            entry->binary_reason = CONCURRENCY_DETERMINISTIC_PROJECTION_COMPLETE;
            continue;
        }
        ConcurrencyProcessEvent event = process->events[next[i]];
        entry->next_kind = event.kind;
        entry->next_projection_index = event.projection_index;
        bool at_head = event.kind == CONCURRENCY_DETERMINISTIC_TASK_EVENT
            ? event.projection_index == binary.next_task_index
            : event.projection_index == binary.next_channel_index;
        if (!at_head) {
            entry->blocked = CONCURRENCY_PROCESS_PREDECESSOR_PENDING;
            continue;
        }
        entry->enabled = event.kind == CONCURRENCY_DETERMINISTIC_TASK_EVENT
            ? binary.task_enabled : binary.channel_enabled;
        entry->binary_reason = event.kind == CONCURRENCY_DETERMINISTIC_TASK_EVENT
            ? binary.task_blocked : binary.channel_blocked;
        entry->blocked = entry->enabled
            ? CONCURRENCY_PROCESS_FRONTIER_ENABLED
            : CONCURRENCY_PROCESS_SEMANTICALLY_BLOCKED;
        if (entry->enabled) frontier->enabled_count++;
    }
    free(events);
    free(next);
    frontier->fingerprint = frontier_fingerprint(frontier);
    return CONCURRENCY_PROCESS_CERTIFIED;
}

static bool frontier_equal(
    const ConcurrencyProcessFrontier *left,
    const ConcurrencyProcessFrontier *right) {
    if (left->process_count != right->process_count ||
        left->enabled_count != right->enabled_count ||
        left->prefix_count != right->prefix_count ||
        left->program_fingerprint != right->program_fingerprint ||
        left->prefix_fingerprint != right->prefix_fingerprint ||
        left->binary.task_enabled != right->binary.task_enabled ||
        left->binary.channel_enabled != right->binary.channel_enabled ||
        left->binary.task_blocked != right->binary.task_blocked ||
        left->binary.channel_blocked != right->binary.channel_blocked ||
        left->binary.local_channel_blocked !=
            right->binary.local_channel_blocked ||
        left->binary.next_task_index != right->binary.next_task_index ||
        left->binary.next_channel_index != right->binary.next_channel_index ||
        left->fingerprint != right->fingerprint)
        return false;
    for (size_t i = 0; i < left->process_count; i++) {
        const ConcurrencyProcessFrontierEntry *a = &left->entries[i];
        const ConcurrencyProcessFrontierEntry *b = &right->entries[i];
        if (!process_equal(a->process, b->process) ||
            a->next_local_index != b->next_local_index ||
            a->enabled != b->enabled || a->blocked != b->blocked ||
            a->binary_reason != b->binary_reason ||
            a->next_kind != b->next_kind ||
            a->next_projection_index != b->next_projection_index)
            return false;
    }
    return true;
}

ConcurrencyProcessStatus concurrency_process_frontier_verify(
    const ConcurrencyDeterministicTrace *oracle,
    const ConcurrencyProcessProgram *program,
    const ConcurrencyProcessSchedule *prefix,
    const ConcurrencyProcessFrontier *frontier) {
    if (!frontier || !program ||
        frontier->process_count != program->process_count ||
        (frontier->process_count && !frontier->entries))
        return CONCURRENCY_PROCESS_INVALID_ARGUMENT;
    if (frontier->fingerprint != frontier_fingerprint(frontier))
        return CONCURRENCY_PROCESS_INVALID_CERTIFICATE;
    ConcurrencyProcessFrontier expected = {0};
    ConcurrencyProcessStatus status = concurrency_process_frontier_build(
        oracle, program, prefix, &expected);
    if (status != CONCURRENCY_PROCESS_CERTIFIED)
        return CONCURRENCY_PROCESS_INVALID_CERTIFICATE;
    bool equal = frontier_equal(frontier, &expected);
    concurrency_process_frontier_free(&expected);
    return equal ? CONCURRENCY_PROCESS_CERTIFIED
                 : CONCURRENCY_PROCESS_INVALID_CERTIFICATE;
}
