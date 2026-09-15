#include "native.h"
#include "dependence.h"

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

static bool native_choice_identity_equal(
    ConcurrencyNativeChoice left, ConcurrencyNativeChoice right);
static uint64_t native_wakeup_obligation_fingerprint(
    const ConcurrencyNativeWakeupObligation *obligation);

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

static const ConcurrencyNativeProcess *find_process(
    const ConcurrencyNativeProgram *program, ConcurrencyProcessId id) {
    for (size_t i = 0; i < program->process_count; i++)
        if (process_equal(program->processes[i].id, id))
            return &program->processes[i];
    return NULL;
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

static uint64_t program_fingerprint(const ConcurrencyNativeProgram *program) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, program->process_count);
    for (size_t i = 0; i < program->process_count; i++) {
        const ConcurrencyNativeProcess *process = &program->processes[i];
        hash = mix(hash, process->id.module_id);
        hash = mix(hash, process->id.process_id);
        hash = mix(hash, process->operation_count);
        for (size_t j = 0; j < process->operation_count; j++) {
            const ConcurrencyNativeOperation *operation = &process->operations[j];
            hash = mix(hash, operation->operation_id);
            hash = mix(hash, operation->task_transition);
            hash = mix(hash, operation->task_step_index);
            hash = mix(hash, operation->network_transition);
            hash = mix(hash, operation->network_step_index);
            hash = mix(hash, operation->read_count);
            for (size_t k = 0; k < operation->read_count; k++)
                hash = mix_place(hash, operation->reads[k]);
            hash = mix(hash, operation->write_count);
            for (size_t k = 0; k < operation->write_count; k++)
                hash = mix_place(hash, operation->writes[k]);
            hash = mix(hash, operation->effect_count);
            for (size_t k = 0; k < operation->effect_count; k++) {
                const QttEffectAtom *atom = &operation->effects[k];
                hash = mix(hash, atom->kind);
                hash = mix(hash, atom->constructor_id);
                hash = mix(hash, atom->type_id);
                hash = mix(hash, atom->capability_id);
                hash = mix(hash, atom->resumption.is_omega);
                hash = mix(hash, atom->resumption.finite);
                hash = mix(hash, atom->scoped);
            }
        }
    }
    if (program->task_model) {
        hash = mix(hash, program->task_model->root.module_id);
        hash = mix(hash, program->task_model->root.task_id);
        hash = mix(hash, program->task_model->step_count);
        for (size_t i = 0; i < program->task_model->step_count; i++) {
            const ConcurrencyStep *step = &program->task_model->steps[i];
            hash = mix(hash, step->kind);
            hash = mix(hash, step->parent.module_id);
            hash = mix(hash, step->parent.task_id);
            hash = mix(hash, step->child.module_id);
            hash = mix(hash, step->child.task_id);
            hash = mix(hash, step->capture_count);
            for (size_t j = 0; j < step->capture_count; j++)
                hash = mix_place(hash, step->captures[j]);
            hash = mix(hash, step->loan_capture_count);
            for (size_t j = 0; j < step->loan_capture_count; j++) {
                hash = mix(hash, step->loan_captures[j].module_id);
                hash = mix(hash, step->loan_captures[j].loan_id);
            }
        }
    }
    if (program->network_model) {
        hash = mix(hash, program->network_model->channel_count);
        for (size_t i = 0; i < program->network_model->channel_count; i++) {
            const ConcurrencyNetworkChannel *channel =
                &program->network_model->channels[i];
            hash = mix(hash, channel->channel_id.module_id);
            hash = mix(hash, channel->channel_id.channel_id);
            hash = mix(hash, channel->capacity);
            hash = mix(hash, channel->scope_capability_id);
            hash = mix(hash, channel->channel_type_id);
        }
        hash = mix(hash, program->network_model->step_count);
        for (size_t i = 0; i < program->network_model->step_count; i++) {
            const ConcurrencyNetworkTraceStep *step =
                &program->network_model->steps[i];
            hash = mix(hash, step->channel_id.module_id);
            hash = mix(hash, step->channel_id.channel_id);
            hash = mix(hash, step->channel_step.kind);
            hash = mix(hash, step->channel_step.actor.module_id);
            hash = mix(hash, step->channel_step.actor.task_id);
            hash = mix(hash, step->channel_step.endpoint_id);
            hash = mix(hash, step->channel_step.peer_actor.module_id);
            hash = mix(hash, step->channel_step.peer_actor.task_id);
            hash = mix(hash, step->channel_step.peer_endpoint_id);
            hash = mix(hash, step->channel_step.payload_type_id);
            hash = mix(hash, step->channel_step.delegated_endpoint_id);
        }
    }
    return hash;
}

static uint64_t prefix_fingerprint(
    const ConcurrencyNativeChoice *prefix, size_t count) {
    uint64_t hash = mix(UINT64_C(1469598103934665603), count);
    for (size_t i = 0; i < count; i++) {
        hash = mix(hash, prefix[i].process.module_id);
        hash = mix(hash, prefix[i].process.process_id);
        hash = mix(hash, prefix[i].local_index);
        hash = mix(hash, prefix[i].tick);
    }
    return hash;
}

static ConcurrencyNativeStatus validate_program(
    const ConcurrencyNativeProgram *program, size_t *total) {
    if (!program || !program->processes || !program->process_count)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *total = 0;
    size_t task_step_count = program->task_model
        ? program->task_model->step_count : 0;
    size_t network_step_count = program->network_model
        ? program->network_model->step_count : 0;
    bool *task_steps = task_step_count
        ? calloc(task_step_count, sizeof(*task_steps)) : NULL;
    bool *network_steps = network_step_count
        ? calloc(network_step_count, sizeof(*network_steps)) : NULL;
    if (task_step_count && (!program->task_model->steps || !task_steps)) {
        free(task_steps);
        free(network_steps);
        return CONCURRENCY_NATIVE_INVALID_PROGRAM;
    }
    if (network_step_count &&
        (!program->network_model->steps || !program->network_model->channels ||
         !program->network_model->channel_count ||
         !program->network_model->authorities || !network_steps)) {
        free(task_steps);
        free(network_steps);
        return CONCURRENCY_NATIVE_INVALID_PROGRAM;
    }
    ConcurrencyNativeStatus status = CONCURRENCY_NATIVE_CERTIFIED;
    for (size_t i = 0; i < program->process_count; i++) {
        const ConcurrencyNativeProcess *process = &program->processes[i];
        if (!process->id.module_id || !process->id.process_id ||
            !process->operations || !process->operation_count)
            { status = CONCURRENCY_NATIVE_INVALID_PROGRAM; break; }
        for (size_t prior = 0; prior < i; prior++)
            if (process_equal(process->id, program->processes[prior].id))
                { status = CONCURRENCY_NATIVE_INVALID_PROGRAM; break; }
        if (status != CONCURRENCY_NATIVE_CERTIFIED) break;
        if (*total > SIZE_MAX - process->operation_count)
            { status = CONCURRENCY_NATIVE_INVALID_PROGRAM; break; }
        *total += process->operation_count;
        for (size_t j = 0; j < process->operation_count; j++) {
            const ConcurrencyNativeOperation *operation = &process->operations[j];
            if (!operation->operation_id ||
                (operation->task_transition && operation->network_transition) ||
                (operation->read_count && !operation->reads) ||
                (operation->write_count && !operation->writes) ||
                (operation->effect_count && !operation->effects))
                { status = CONCURRENCY_NATIVE_INVALID_PROGRAM; break; }
            if (operation->task_transition) {
                if (!program->task_model ||
                    operation->task_step_index >= task_step_count ||
                    task_steps[operation->task_step_index]) {
                    status = CONCURRENCY_NATIVE_INVALID_PROGRAM;
                    break;
                }
                task_steps[operation->task_step_index] = true;
            }
            if (operation->network_transition) {
                if (!program->network_model ||
                    operation->network_step_index >= network_step_count ||
                    network_steps[operation->network_step_index]) {
                    status = CONCURRENCY_NATIVE_INVALID_PROGRAM;
                    break;
                }
                network_steps[operation->network_step_index] = true;
            }
            for (size_t previous_process = 0;
                 previous_process <= i; previous_process++) {
                size_t prior_limit = previous_process == i ? j
                    : program->processes[previous_process].operation_count;
                for (size_t previous = 0; previous < prior_limit; previous++)
                    if (operation->operation_id ==
                        program->processes[previous_process]
                            .operations[previous].operation_id)
                        { status = CONCURRENCY_NATIVE_INVALID_PROGRAM; break; }
                if (status != CONCURRENCY_NATIVE_CERTIFIED) break;
            }
            if (status != CONCURRENCY_NATIVE_CERTIFIED) break;
            for (size_t k = 0; k < operation->read_count; k++)
                if (!place_valid(operation->reads[k]))
                    { status = CONCURRENCY_NATIVE_INVALID_PROGRAM; break; }
            if (status != CONCURRENCY_NATIVE_CERTIFIED) break;
            for (size_t k = 0; k < operation->write_count; k++)
                if (!place_valid(operation->writes[k]))
                    { status = CONCURRENCY_NATIVE_INVALID_PROGRAM; break; }
        }
        if (status != CONCURRENCY_NATIVE_CERTIFIED) break;
    }
    if (status == CONCURRENCY_NATIVE_CERTIFIED)
        for (size_t i = 0; i < task_step_count; i++)
            if (!task_steps[i]) {
                status = CONCURRENCY_NATIVE_INVALID_PROGRAM;
                break;
            }
    if (status == CONCURRENCY_NATIVE_CERTIFIED)
        for (size_t i = 0; i < network_step_count; i++)
            if (!network_steps[i]) {
                status = CONCURRENCY_NATIVE_INVALID_PROGRAM;
                break;
            }
    free(task_steps);
    free(network_steps);
    return status;
}

static ConcurrencyNativeStatus derive_cursors(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    size_t *cursors) {
    if (prefix_count && !prefix) return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
    uint64_t previous_tick = 0;
    for (size_t i = 0; i < prefix_count; i++) {
        const ConcurrencyNativeProcess *process = find_process(
            program, prefix[i].process);
        if (!process) return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
        size_t index = (size_t)(process - program->processes);
        if (prefix[i].local_index != cursors[index] ||
            cursors[index] >= process->operation_count ||
            (i && prefix[i].tick < previous_tick))
            return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
        previous_tick = prefix[i].tick;
        cursors[index]++;
    }
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static uint64_t frontier_fingerprint(const ConcurrencyNativeFrontier *frontier) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, frontier->process_count);
    hash = mix(hash, frontier->enabled_count);
    hash = mix(hash, frontier->prefix_count);
    hash = mix(hash, frontier->program_fingerprint);
    hash = mix(hash, frontier->prefix_fingerprint);
    for (size_t i = 0; i < frontier->process_count; i++) {
        const ConcurrencyNativeFrontierEntry *entry = &frontier->entries[i];
        hash = mix(hash, entry->process.module_id);
        hash = mix(hash, entry->process.process_id);
        hash = mix(hash, entry->local_index);
        hash = mix(hash, entry->operation_id);
        hash = mix(hash, entry->enabled);
    }
    return hash;
}

ConcurrencyNativeOperation concurrency_native_operation(uint64_t operation_id) {
    return (ConcurrencyNativeOperation){.operation_id = operation_id};
}

ConcurrencyNativeOperation concurrency_native_task_operation(
    uint64_t operation_id, size_t task_step_index) {
    return (ConcurrencyNativeOperation){
        .operation_id = operation_id,
        .task_transition = true,
        .task_step_index = task_step_index,
    };
}

ConcurrencyNativeOperation concurrency_native_network_operation(
    uint64_t operation_id, size_t network_step_index) {
    return (ConcurrencyNativeOperation){
        .operation_id = operation_id,
        .network_transition = true,
        .network_step_index = network_step_index,
    };
}

static ConcurrencyNativeStatus task_candidate_enabled(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeOperation *candidate, bool *enabled) {
    *enabled = true;
    if (!candidate->task_transition) return CONCURRENCY_NATIVE_CERTIFIED;
    if (prefix_count > SIZE_MAX - 1 ||
        prefix_count + 1 > SIZE_MAX / sizeof(ConcurrencyStep))
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    ConcurrencyStep *steps = malloc((prefix_count + 1) * sizeof(*steps));
    if (!steps) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    size_t count = 0;
    for (size_t i = 0; i < prefix_count; i++) {
        const ConcurrencyNativeProcess *process = find_process(
            program, prefix[i].process);
        if (!process || prefix[i].local_index >= process->operation_count) {
            free(steps);
            return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
        }
        const ConcurrencyNativeOperation *operation =
            &process->operations[prefix[i].local_index];
        if (operation->task_transition)
            steps[count++] =
                program->task_model->steps[operation->task_step_index];
    }
    steps[count++] = program->task_model->steps[candidate->task_step_index];
    ConcurrencyTrace trace = *program->task_model;
    trace.steps = steps;
    trace.step_count = count;
    ConcurrencyVerification verification = concurrency_verify_trace_prefix(&trace);
    free(steps);
    if (verification.error == CONCURRENCY_OUT_OF_MEMORY)
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    *enabled = verification.error == CONCURRENCY_VALID;
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static ConcurrencyNativeStatus network_candidate_enabled(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeOperation *candidate, bool *enabled) {
    *enabled = true;
    if (!candidate->network_transition) return CONCURRENCY_NATIVE_CERTIFIED;
    if (prefix_count > SIZE_MAX - 1 ||
        prefix_count + 1 > SIZE_MAX / sizeof(ConcurrencyNetworkTraceStep))
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    ConcurrencyNetworkTraceStep *steps = malloc(
        (prefix_count + 1) * sizeof(*steps));
    if (!steps) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    size_t count = 0;
    for (size_t i = 0; i < prefix_count; i++) {
        const ConcurrencyNativeProcess *process = find_process(
            program, prefix[i].process);
        if (!process || prefix[i].local_index >= process->operation_count) {
            free(steps);
            return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
        }
        const ConcurrencyNativeOperation *operation =
            &process->operations[prefix[i].local_index];
        if (operation->network_transition)
            steps[count++] = program->network_model->steps[
                operation->network_step_index];
    }
    steps[count++] = program->network_model->steps[
        candidate->network_step_index];
    ConcurrencyNetworkTrace trace = *program->network_model;
    trace.steps = steps;
    trace.step_count = count;
    ConcurrencyChannelMachineResult result = {0};
    ConcurrencyDeterministicStatus status =
        concurrency_deterministic_channel_enabled(&trace, count - 1, &result);
    free(steps);
    if (status != CONCURRENCY_DETERMINISTIC_CERTIFIED) {
        *enabled = false;
        return CONCURRENCY_NATIVE_CERTIFIED;
    }
    *enabled = result.enabled;
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static ConcurrencyNativeStatus temporal_candidate_enabled(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeOperation *candidate, bool *enabled) {
    if (!*enabled || !program->task_model || !program->network_model ||
        (!candidate->task_transition && !candidate->network_transition))
        return CONCURRENCY_NATIVE_CERTIFIED;
    if (prefix_count > SIZE_MAX - 1 ||
        prefix_count + 1 > SIZE_MAX / sizeof(ConcurrencyDeterministicEvent))
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    ConcurrencyDeterministicEvent *selected = malloc(
        (prefix_count + 1) * sizeof(*selected));
    if (!selected) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    size_t count = 0;
    for (size_t i = 0; i < prefix_count; i++) {
        const ConcurrencyNativeProcess *process = find_process(
            program, prefix[i].process);
        if (!process || prefix[i].local_index >= process->operation_count) {
            free(selected);
            return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
        }
        const ConcurrencyNativeOperation *operation =
            &process->operations[prefix[i].local_index];
        if (operation->task_transition)
            selected[count++] = concurrency_deterministic_task_event(
                operation->task_step_index, prefix[i].tick);
        else if (operation->network_transition)
            selected[count++] = concurrency_deterministic_channel_event(
                operation->network_step_index, prefix[i].tick);
    }
    ConcurrencyDeterministicEvent event = candidate->task_transition
        ? concurrency_deterministic_task_event(candidate->task_step_index, 0)
        : concurrency_deterministic_channel_event(
              candidate->network_step_index, 0);
    ConcurrencyDeterministicBlockReason blocked =
        CONCURRENCY_DETERMINISTIC_ENABLED;
    ConcurrencyDeterministicStatus status =
        concurrency_deterministic_temporal_enabled(
            program->task_model, program->network_model,
            selected, count, event, enabled, &blocked);
    free(selected);
    return status == CONCURRENCY_DETERMINISTIC_CERTIFIED
        ? CONCURRENCY_NATIVE_CERTIFIED
        : CONCURRENCY_NATIVE_INVALID_PROGRAM;
}

void concurrency_native_frontier_free(ConcurrencyNativeFrontier *frontier) {
    if (!frontier) return;
    free(frontier->entries);
    *frontier = (ConcurrencyNativeFrontier){0};
}

ConcurrencyNativeStatus concurrency_native_frontier(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    ConcurrencyNativeFrontier *frontier) {
    if (!frontier) return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *frontier = (ConcurrencyNativeFrontier){0};
    size_t total = 0;
    ConcurrencyNativeStatus status = validate_program(program, &total);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    if (prefix_count > total ||
        program->process_count > SIZE_MAX / sizeof(size_t) ||
        program->process_count >
            SIZE_MAX / sizeof(ConcurrencyNativeFrontierEntry))
        return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
    size_t *cursors = calloc(program->process_count, sizeof(*cursors));
    ConcurrencyNativeFrontierEntry *entries = calloc(
        program->process_count, sizeof(*entries));
    if (!cursors || !entries) {
        free(cursors);
        free(entries);
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    }
    status = derive_cursors(program, prefix, prefix_count, cursors);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) {
        free(cursors);
        free(entries);
        return status;
    }
    frontier->entries = entries;
    frontier->process_count = program->process_count;
    frontier->prefix_count = prefix_count;
    frontier->program_fingerprint = program_fingerprint(program);
    frontier->prefix_fingerprint = prefix_fingerprint(prefix, prefix_count);
    for (size_t i = 0; i < program->process_count; i++) {
        entries[i].process = program->processes[i].id;
        entries[i].local_index = cursors[i];
        entries[i].enabled = cursors[i] < program->processes[i].operation_count;
        if (entries[i].enabled) {
            const ConcurrencyNativeOperation *operation =
                &program->processes[i].operations[cursors[i]];
            entries[i].operation_id = operation->operation_id;
            status = task_candidate_enabled(
                program, prefix, prefix_count, operation, &entries[i].enabled);
            if (status != CONCURRENCY_NATIVE_CERTIFIED) {
                free(cursors);
                concurrency_native_frontier_free(frontier);
                return status;
            }
            status = temporal_candidate_enabled(
                program, prefix, prefix_count, operation,
                &entries[i].enabled);
            if (status != CONCURRENCY_NATIVE_CERTIFIED) {
                free(cursors);
                concurrency_native_frontier_free(frontier);
                return status;
            }
            if (entries[i].enabled) {
                status = network_candidate_enabled(
                    program, prefix, prefix_count, operation,
                    &entries[i].enabled);
                if (status != CONCURRENCY_NATIVE_CERTIFIED) {
                    free(cursors);
                    concurrency_native_frontier_free(frontier);
                    return status;
                }
            }
            if (entries[i].enabled) frontier->enabled_count++;
        }
    }
    free(cursors);
    frontier->fingerprint = frontier_fingerprint(frontier);
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static bool frontier_equal(
    const ConcurrencyNativeFrontier *left,
    const ConcurrencyNativeFrontier *right) {
    if (left->process_count != right->process_count ||
        left->enabled_count != right->enabled_count ||
        left->prefix_count != right->prefix_count ||
        left->program_fingerprint != right->program_fingerprint ||
        left->prefix_fingerprint != right->prefix_fingerprint ||
        left->fingerprint != right->fingerprint)
        return false;
    for (size_t i = 0; i < left->process_count; i++)
        if (!process_equal(left->entries[i].process, right->entries[i].process) ||
            left->entries[i].local_index != right->entries[i].local_index ||
            left->entries[i].operation_id != right->entries[i].operation_id ||
            left->entries[i].enabled != right->entries[i].enabled)
            return false;
    return true;
}

ConcurrencyNativeStatus concurrency_native_frontier_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeFrontier *frontier) {
    if (!frontier || !program ||
        frontier->process_count != program->process_count ||
        (frontier->process_count && !frontier->entries))
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    if (frontier->fingerprint != frontier_fingerprint(frontier))
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    ConcurrencyNativeFrontier expected = {0};
    ConcurrencyNativeStatus status = concurrency_native_frontier(
        program, prefix, prefix_count, &expected);
    if (status != CONCURRENCY_NATIVE_CERTIFIED)
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    bool equal = frontier_equal(frontier, &expected);
    concurrency_native_frontier_free(&expected);
    return equal ? CONCURRENCY_NATIVE_CERTIFIED
                 : CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
}

static bool effect_writes(QttEffectKind kind) {
    return kind == QTT_EFFECT_STATE || kind == QTT_EFFECT_WRITE ||
           kind == QTT_EFFECT_ALLOCATE || kind == QTT_EFFECT_IO ||
           kind == QTT_EFFECT_FOREIGN || kind == QTT_EFFECT_ASYNC;
}

static bool places_overlap(
    const QttPlace *left, size_t left_count,
    const QttPlace *right, size_t right_count) {
    for (size_t i = 0; i < left_count; i++)
        for (size_t j = 0; j < right_count; j++)
            if (qtt_place_overlaps(left[i], right[j])) return true;
    return false;
}

static void operation_dependence(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeOperation *left,
    const ConcurrencyNativeOperation *right,
    ConcurrencyNativeDependence *dependence) {
    if ((left->task_transition || left->network_transition) &&
        (right->task_transition || right->network_transition)) {
        ConcurrencyTrace empty_tasks = {0};
        ConcurrencyNetworkTrace empty_network = {0};
        ConcurrencyDeterministicTrace trace = {
            .task_trace = program->task_model
                ? program->task_model : &empty_tasks,
            .network_trace = program->network_model
                ? program->network_model : &empty_network,
        };
        ConcurrencyDependence task = {0};
        ConcurrencyDeterministicEvent left_event = {
            .kind = left->task_transition
                ? CONCURRENCY_DETERMINISTIC_TASK_EVENT
                : CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT,
            .projection_index = left->task_transition
                ? left->task_step_index : left->network_step_index,
        };
        ConcurrencyDeterministicEvent right_event = {
            .kind = right->task_transition
                ? CONCURRENCY_DETERMINISTIC_TASK_EVENT
                : CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT,
            .projection_index = right->task_transition
                ? right->task_step_index : right->network_step_index,
        };
        if (concurrency_deterministic_dependence(
                &trace, left_event, right_event, &task) ==
            CONCURRENCY_DETERMINISTIC_CERTIFIED) {
            if (task.reasons & CONCURRENCY_DEPENDENCE_TASK_ORDER)
                dependence->reasons |=
                    CONCURRENCY_NATIVE_DEPENDENCE_TASK_ORDER;
            if (task.reasons & CONCURRENCY_DEPENDENCE_TASK_LIFETIME)
                dependence->reasons |=
                    CONCURRENCY_NATIVE_DEPENDENCE_TASK_LIFETIME;
            if (task.reasons & CONCURRENCY_DEPENDENCE_CLEANUP)
                dependence->reasons |=
                    CONCURRENCY_NATIVE_DEPENDENCE_CLEANUP;
            if (task.reasons & CONCURRENCY_DEPENDENCE_AUTHORITY)
                dependence->reasons |=
                    CONCURRENCY_NATIVE_DEPENDENCE_AUTHORITY;
            if (task.reasons & CONCURRENCY_DEPENDENCE_CHANNEL)
                dependence->reasons |=
                    CONCURRENCY_NATIVE_DEPENDENCE_CHANNEL;
            if (task.reasons & CONCURRENCY_DEPENDENCE_EFFECT)
                dependence->reasons |=
                    CONCURRENCY_NATIVE_DEPENDENCE_EFFECT_CAPABILITY;
        }
    }
    if (places_overlap(left->writes, left->write_count,
                       right->writes, right->write_count) ||
        places_overlap(left->writes, left->write_count,
                       right->reads, right->read_count) ||
        places_overlap(left->reads, left->read_count,
                       right->writes, right->write_count))
        dependence->reasons |= CONCURRENCY_NATIVE_DEPENDENCE_QTT_PLACE;
    for (size_t i = 0; i < left->effect_count; i++)
        for (size_t j = 0; j < right->effect_count; j++) {
            const QttEffectAtom *a = &left->effects[i];
            const QttEffectAtom *b = &right->effects[j];
            if (a->capability_id && a->capability_id == b->capability_id &&
                (effect_writes(a->kind) || effect_writes(b->kind)))
                dependence->reasons |=
                    CONCURRENCY_NATIVE_DEPENDENCE_EFFECT_CAPABILITY;
        }
    dependence->dependent = dependence->reasons != 0;
}

ConcurrencyNativeStatus concurrency_native_dependence(
    const ConcurrencyNativeProgram *program,
    ConcurrencyProcessId left, ConcurrencyProcessId right,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    ConcurrencyNativeDependence *dependence) {
    if (!dependence) return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *dependence = (ConcurrencyNativeDependence){0};
    ConcurrencyNativeFrontier frontier = {0};
    ConcurrencyNativeStatus status = concurrency_native_frontier(
        program, prefix, prefix_count, &frontier);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    const ConcurrencyNativeFrontierEntry *a = NULL;
    const ConcurrencyNativeFrontierEntry *b = NULL;
    size_t ai = 0;
    size_t bi = 0;
    for (size_t i = 0; i < frontier.process_count; i++) {
        if (process_equal(frontier.entries[i].process, left)) {
            a = &frontier.entries[i];
            ai = i;
        }
        if (process_equal(frontier.entries[i].process, right)) {
            b = &frontier.entries[i];
            bi = i;
        }
    }
    if (!a || !b || !a->enabled || !b->enabled) {
        concurrency_native_frontier_free(&frontier);
        return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
    }
    if (process_equal(left, right)) {
        dependence->dependent = true;
        dependence->reasons = CONCURRENCY_NATIVE_DEPENDENCE_PROCESS_ORDER;
    } else {
        operation_dependence(program,
            &program->processes[ai].operations[a->local_index],
            &program->processes[bi].operations[b->local_index], dependence);
    }
    concurrency_native_frontier_free(&frontier);
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static uint64_t native_process_set_fingerprint(
    const ConcurrencyProcessId *processes, size_t count) {
    uint64_t hash = mix(UINT64_C(1469598103934665603), count);
    for (size_t i = 0; i < count; i++) {
        hash = mix(hash, processes[i].module_id);
        hash = mix(hash, processes[i].process_id);
    }
    return hash;
}

static uint64_t native_sleep_step_fingerprint(
    const ConcurrencyNativeSleepStepCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->selected.module_id);
    hash = mix(hash, certificate->selected.process_id);
    hash = mix(hash, certificate->input_count);
    hash = mix(hash, certificate->retained_count);
    hash = mix(hash, certificate->program_fingerprint);
    hash = mix(hash, certificate->prefix_fingerprint);
    hash = mix(hash, certificate->input_fingerprint);
    for (size_t i = 0; i < certificate->input_count; i++) {
        const ConcurrencyNativeSleepDecision *decision =
            &certificate->decisions[i];
        hash = mix(hash, decision->process.module_id);
        hash = mix(hash, decision->process.process_id);
        hash = mix(hash, decision->retained);
        hash = mix(hash, decision->reasons);
    }
    for (size_t i = 0; i < certificate->retained_count; i++) {
        hash = mix(hash, certificate->retained[i].module_id);
        hash = mix(hash, certificate->retained[i].process_id);
    }
    return hash;
}

void concurrency_native_sleep_step_free(
    ConcurrencyNativeSleepStepCertificate *certificate) {
    if (!certificate) return;
    free(certificate->decisions);
    free(certificate->retained);
    *certificate = (ConcurrencyNativeSleepStepCertificate){0};
}

ConcurrencyNativeStatus concurrency_native_sleep_step(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    ConcurrencyProcessId selected,
    ConcurrencyNativeSleepStepCertificate *certificate) {
    if (!program || !certificate || (prefix_count && !prefix) ||
        (sleep_count && !sleep) || !selected.module_id ||
        !selected.process_id)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *certificate = (ConcurrencyNativeSleepStepCertificate){0};
    for (size_t i = 0; i < sleep_count; i++) {
        if (!sleep[i].module_id || !sleep[i].process_id ||
            process_equal(sleep[i], selected))
            return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
        for (size_t prior = 0; prior < i; prior++)
            if (process_equal(sleep[i], sleep[prior]))
                return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    }
    ConcurrencyNativeFrontier frontier = {0};
    ConcurrencyNativeStatus status = concurrency_native_frontier(
        program, prefix, prefix_count, &frontier);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    bool selected_enabled = false;
    for (size_t i = 0; i < frontier.process_count; i++)
        if (process_equal(frontier.entries[i].process, selected))
            selected_enabled = frontier.entries[i].enabled;
    concurrency_native_frontier_free(&frontier);
    if (!selected_enabled) return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
    *certificate = (ConcurrencyNativeSleepStepCertificate){
        .selected = selected,
        .input_count = sleep_count,
        .program_fingerprint = program_fingerprint(program),
        .prefix_fingerprint = prefix_fingerprint(prefix, prefix_count),
        .input_fingerprint = native_process_set_fingerprint(sleep, sleep_count),
    };
    if (sleep_count > SIZE_MAX / sizeof(*certificate->decisions) ||
        sleep_count > SIZE_MAX / sizeof(*certificate->retained))
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    certificate->decisions = sleep_count
        ? calloc(sleep_count, sizeof(*certificate->decisions)) : NULL;
    certificate->retained = sleep_count
        ? malloc(sleep_count * sizeof(*certificate->retained)) : NULL;
    if (sleep_count && (!certificate->decisions || !certificate->retained)) {
        concurrency_native_sleep_step_free(certificate);
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < sleep_count; i++) {
        ConcurrencyNativeDependence dependence = {0};
        status = concurrency_native_dependence(
            program, selected, sleep[i], prefix, prefix_count, &dependence);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) goto fail;
        certificate->decisions[i] = (ConcurrencyNativeSleepDecision){
            .process = sleep[i],
            .retained = !dependence.dependent,
            .reasons = dependence.reasons,
        };
        if (!dependence.dependent)
            certificate->retained[certificate->retained_count++] = sleep[i];
    }
    certificate->fingerprint = native_sleep_step_fingerprint(certificate);
    return CONCURRENCY_NATIVE_CERTIFIED;
fail:
    concurrency_native_sleep_step_free(certificate);
    return status;
}

ConcurrencyNativeStatus concurrency_native_sleep_step_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    ConcurrencyProcessId selected,
    const ConcurrencyNativeSleepStepCertificate *certificate) {
    if (!certificate || sleep_count != certificate->input_count ||
        (sleep_count && (!sleep || !certificate->decisions)) ||
        (certificate->retained_count && !certificate->retained))
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    if (certificate->retained_count > certificate->input_count ||
        !process_equal(selected, certificate->selected) ||
        certificate->fingerprint != native_sleep_step_fingerprint(certificate))
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    ConcurrencyNativeSleepStepCertificate expected = {0};
    ConcurrencyNativeStatus status = concurrency_native_sleep_step(
        program, prefix, prefix_count, sleep, sleep_count, selected, &expected);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    bool equal = certificate->fingerprint == expected.fingerprint;
    concurrency_native_sleep_step_free(&expected);
    return equal ? CONCURRENCY_NATIVE_CERTIFIED
                 : CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
}

static uint64_t native_race_fingerprint(const ConcurrencyNativeRace *race) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, race->earlier_index);
    hash = mix(hash, race->later_index);
    hash = mix(hash, race->reasons);
    hash = mix(hash, race->reversal_prefix_count);
    hash = mix(hash, race->reversal_count);
    for (size_t i = 0; i < race->reversal_count; i++) {
        hash = mix(hash, race->reversal[i].process.module_id);
        hash = mix(hash, race->reversal[i].process.process_id);
        hash = mix(hash, race->reversal[i].local_index);
        hash = mix(hash, race->reversal[i].tick);
    }
    return hash;
}

static uint64_t native_races_fingerprint(
    const ConcurrencyNativeRaceCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->occurrence_count);
    hash = mix(hash, certificate->happens_before_edge_count);
    hash = mix(hash, certificate->race_count);
    hash = mix(hash, certificate->program_fingerprint);
    hash = mix(hash, certificate->schedule_fingerprint);
    size_t matrix_count = certificate->occurrence_count *
        certificate->occurrence_count;
    for (size_t i = 0; i < matrix_count; i++)
        hash = mix(hash, certificate->happens_before[i]);
    for (size_t i = 0; i < certificate->race_count; i++)
        hash = mix(hash, certificate->races[i].fingerprint);
    return hash;
}

void concurrency_native_races_free(
    ConcurrencyNativeRaceCertificate *certificate) {
    if (!certificate) return;
    for (size_t i = 0; i < certificate->race_count; i++)
        free(certificate->races[i].reversal);
    free(certificate->races);
    free(certificate->happens_before);
    *certificate = (ConcurrencyNativeRaceCertificate){0};
}

bool concurrency_native_happens_before(
    const ConcurrencyNativeRaceCertificate *certificate,
    size_t earlier_index, size_t later_index) {
    return certificate && certificate->happens_before &&
           earlier_index < certificate->occurrence_count &&
           later_index < certificate->occurrence_count &&
           certificate->happens_before[
               earlier_index * certificate->occurrence_count + later_index];
}

static bool native_choice_enabled(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *schedule, size_t prefix_count,
    ConcurrencyNativeChoice choice) {
    ConcurrencyNativeFrontier frontier = {0};
    if (concurrency_native_frontier(
            program, schedule, prefix_count, &frontier) !=
        CONCURRENCY_NATIVE_CERTIFIED)
        return false;
    bool enabled = false;
    for (size_t i = 0; i < frontier.process_count; i++)
        if (process_equal(frontier.entries[i].process, choice.process) &&
            frontier.entries[i].local_index == choice.local_index) {
            enabled = frontier.entries[i].enabled;
            break;
        }
    concurrency_native_frontier_free(&frontier);
    return enabled;
}

static ConcurrencyNativeStatus native_append_race(
    ConcurrencyNativeRaceCertificate *certificate,
    const ConcurrencyNativeRace *race) {
    size_t count = certificate->race_count + 1;
    if (count > SIZE_MAX / sizeof(*certificate->races))
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    ConcurrencyNativeRace *grown = realloc(
        certificate->races, count * sizeof(*grown));
    if (!grown) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    certificate->races = grown;
    grown[count - 1] = *race;
    certificate->race_count = count;
    return CONCURRENCY_NATIVE_CERTIFIED;
}

ConcurrencyNativeStatus concurrency_native_races(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *schedule, size_t schedule_count,
    ConcurrencyNativeRaceCertificate *certificate) {
    if (!program || !schedule || !schedule_count || !certificate ||
        schedule_count > SIZE_MAX / schedule_count)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *certificate = (ConcurrencyNativeRaceCertificate){0};
    size_t total = 0;
    ConcurrencyNativeStatus status = validate_program(program, &total);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    if (schedule_count > total)
        return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
    for (size_t i = 0; i < schedule_count; i++)
        if (!native_choice_enabled(program, schedule, i, schedule[i]))
            return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
    certificate->occurrence_count = schedule_count;
    certificate->program_fingerprint = program_fingerprint(program);
    certificate->schedule_fingerprint = prefix_fingerprint(
        schedule, schedule_count);
    size_t matrix_count = schedule_count * schedule_count;
    certificate->happens_before = calloc(
        matrix_count, sizeof(*certificate->happens_before));
    if (!certificate->happens_before) {
        status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
        goto fail;
    }
    for (size_t i = 0; i < schedule_count; i++) {
        const ConcurrencyNativeProcess *left_process = find_process(
            program, schedule[i].process);
        const ConcurrencyNativeOperation *left =
            &left_process->operations[schedule[i].local_index];
        for (size_t j = i + 1; j < schedule_count; j++) {
            const ConcurrencyNativeProcess *right_process = find_process(
                program, schedule[j].process);
            bool ordered = process_equal(
                schedule[i].process, schedule[j].process);
            if (!ordered) {
                ConcurrencyNativeDependence dependence = {0};
                operation_dependence(
                    program, left,
                    &right_process->operations[schedule[j].local_index],
                    &dependence);
                ordered = dependence.dependent;
            }
            certificate->happens_before[i * schedule_count + j] = ordered;
        }
    }
    for (size_t k = 0; k < schedule_count; k++)
        for (size_t i = 0; i < k; i++) {
            if (!certificate->happens_before[i * schedule_count + k])
                continue;
            for (size_t j = k + 1; j < schedule_count; j++)
                if (certificate->happens_before[k * schedule_count + j])
                    certificate->happens_before[
                        i * schedule_count + j] = 1;
        }
    for (size_t i = 0; i < matrix_count; i++)
        if (certificate->happens_before[i])
            certificate->happens_before_edge_count++;
    for (size_t earlier = 0; earlier < schedule_count; earlier++)
        for (size_t later = earlier + 1; later < schedule_count; later++) {
            if (process_equal(schedule[earlier].process,
                              schedule[later].process) ||
                !certificate->happens_before[
                    earlier * schedule_count + later])
                continue;
            bool cover = true;
            for (size_t middle = earlier + 1; middle < later; middle++)
                if (certificate->happens_before[
                        earlier * schedule_count + middle] &&
                    certificate->happens_before[
                        middle * schedule_count + later]) {
                    cover = false;
                    break;
                }
            if (!cover || !native_choice_enabled(
                    program, schedule, earlier, schedule[later]))
                continue;
            ConcurrencyNativeDependence dependence = {0};
            const ConcurrencyNativeProcess *a = find_process(
                program, schedule[earlier].process);
            const ConcurrencyNativeProcess *b = find_process(
                program, schedule[later].process);
            operation_dependence(
                program, &a->operations[schedule[earlier].local_index],
                &b->operations[schedule[later].local_index], &dependence);
            ConcurrencyNativeRace race = {
                .earlier_index = earlier,
                .later_index = later,
                .reasons = dependence.reasons,
                .reversal_prefix_count = earlier,
            };
            for (size_t middle = earlier + 1; middle < later; middle++)
                if (!certificate->happens_before[
                        earlier * schedule_count + middle])
                    race.reversal_count++;
            race.reversal_count++;
            race.reversal = malloc(
                race.reversal_count * sizeof(*race.reversal));
            if (!race.reversal) {
                status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
                goto fail;
            }
            size_t at = 0;
            for (size_t middle = earlier + 1; middle < later; middle++)
                if (!certificate->happens_before[
                        earlier * schedule_count + middle])
                    race.reversal[at++] = schedule[middle];
            race.reversal[at] = schedule[later];
            race.fingerprint = native_race_fingerprint(&race);
            status = native_append_race(certificate, &race);
            if (status != CONCURRENCY_NATIVE_CERTIFIED) {
                free(race.reversal);
                goto fail;
            }
        }
    certificate->fingerprint = native_races_fingerprint(certificate);
    return CONCURRENCY_NATIVE_CERTIFIED;
fail:
    concurrency_native_races_free(certificate);
    return status;
}

static bool native_races_equal(
    const ConcurrencyNativeRaceCertificate *left,
    const ConcurrencyNativeRaceCertificate *right) {
    if (left->occurrence_count != right->occurrence_count ||
        left->happens_before_edge_count != right->happens_before_edge_count ||
        left->race_count != right->race_count ||
        left->program_fingerprint != right->program_fingerprint ||
        left->schedule_fingerprint != right->schedule_fingerprint ||
        left->fingerprint != right->fingerprint)
        return false;
    size_t matrix_count = left->occurrence_count * left->occurrence_count;
    if (memcmp(left->happens_before, right->happens_before,
               matrix_count * sizeof(*left->happens_before)) != 0)
        return false;
    for (size_t i = 0; i < left->race_count; i++) {
        const ConcurrencyNativeRace *a = &left->races[i];
        const ConcurrencyNativeRace *b = &right->races[i];
        if (a->earlier_index != b->earlier_index ||
            a->later_index != b->later_index || a->reasons != b->reasons ||
            a->reversal_prefix_count != b->reversal_prefix_count ||
            a->reversal_count != b->reversal_count ||
            a->fingerprint != b->fingerprint)
            return false;
        for (size_t j = 0; j < a->reversal_count; j++)
            if (!native_choice_identity_equal(
                    a->reversal[j], b->reversal[j]) ||
                a->reversal[j].tick != b->reversal[j].tick)
                return false;
    }
    return true;
}

ConcurrencyNativeStatus concurrency_native_races_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *schedule, size_t schedule_count,
    const ConcurrencyNativeRaceCertificate *certificate) {
    if (!certificate || !schedule_count ||
        schedule_count > SIZE_MAX / schedule_count ||
        certificate->occurrence_count != schedule_count ||
        !certificate->happens_before ||
        (certificate->race_count && !certificate->races))
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    if (certificate->race_count >
        schedule_count * (schedule_count - 1) / 2)
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    for (size_t i = 0; i < certificate->race_count; i++) {
        const ConcurrencyNativeRace *race = &certificate->races[i];
        if (race->earlier_index >= race->later_index ||
            race->later_index >= schedule_count ||
            race->reversal_prefix_count != race->earlier_index ||
            !race->reversal_count || race->reversal_count > schedule_count ||
            !race->reversal)
            return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    }
    if (certificate->fingerprint != native_races_fingerprint(certificate))
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    ConcurrencyNativeRaceCertificate expected = {0};
    ConcurrencyNativeStatus status = concurrency_native_races(
        program, schedule, schedule_count, &expected);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    bool equal = native_races_equal(certificate, &expected);
    concurrency_native_races_free(&expected);
    return equal ? CONCURRENCY_NATIVE_CERTIFIED
                 : CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
}

static uint64_t native_state_node_fingerprint(
    const ConcurrencyNativeFrontier *frontier) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, frontier->process_count);
    hash = mix(hash, frontier->enabled_count);
    for (size_t i = 0; i < frontier->process_count; i++) {
        const ConcurrencyNativeFrontierEntry *entry = &frontier->entries[i];
        hash = mix(hash, entry->process.module_id);
        hash = mix(hash, entry->process.process_id);
        hash = mix(hash, entry->local_index);
        hash = mix(hash, entry->operation_id);
        hash = mix(hash, entry->enabled);
    }
    return hash;
}

static ConcurrencyNativeCommutationStatus native_future_fingerprint(
    const ConcurrencyNativeProgram *program,
    ConcurrencyNativeChoice *prefix, size_t prefix_count,
    size_t total, size_t max_states, size_t *state_count,
    uint64_t *fingerprint) {
    if (*state_count >= max_states)
        return CONCURRENCY_NATIVE_COMMUTATION_LIMIT_EXCEEDED;
    (*state_count)++;
    ConcurrencyNativeFrontier frontier = {0};
    ConcurrencyNativeStatus native_status = concurrency_native_frontier(
        program, prefix, prefix_count, &frontier);
    if (native_status == CONCURRENCY_NATIVE_OUT_OF_MEMORY)
        return CONCURRENCY_NATIVE_COMMUTATION_OUT_OF_MEMORY;
    if (native_status != CONCURRENCY_NATIVE_CERTIFIED)
        return CONCURRENCY_NATIVE_COMMUTATION_INVALID_ARGUMENT;
    uint64_t hash = native_state_node_fingerprint(&frontier);
    hash = mix(hash, prefix_count == total);
    hash = mix(hash, !frontier.enabled_count && prefix_count < total);
    for (size_t i = 0; i < frontier.process_count; i++) {
        if (!frontier.entries[i].enabled) continue;
        prefix[prefix_count] = (ConcurrencyNativeChoice){
            .process = frontier.entries[i].process,
            .local_index = frontier.entries[i].local_index,
            .tick = prefix_count,
        };
        uint64_t child = 0;
        ConcurrencyNativeCommutationStatus status = native_future_fingerprint(
            program, prefix, prefix_count + 1, total, max_states,
            state_count, &child);
        if (status != CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED) {
            concurrency_native_frontier_free(&frontier);
            return status;
        }
        hash = mix(hash, frontier.entries[i].operation_id);
        hash = mix(hash, child);
    }
    concurrency_native_frontier_free(&frontier);
    *fingerprint = hash;
    return CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED;
}

static bool frontier_process_enabled(
    const ConcurrencyNativeFrontier *frontier, ConcurrencyProcessId process,
    uint64_t *operation_id) {
    for (size_t i = 0; i < frontier->process_count; i++)
        if (process_equal(frontier->entries[i].process, process)) {
            if (operation_id)
                *operation_id = frontier->entries[i].operation_id;
            return frontier->entries[i].enabled;
        }
    return false;
}

static uint64_t native_commutation_certificate_fingerprint(
    const ConcurrencyNativeCommutationCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->prefix_count);
    hash = mix(hash, certificate->max_states);
    hash = mix(hash, certificate->left.module_id);
    hash = mix(hash, certificate->left.process_id);
    hash = mix(hash, certificate->right.module_id);
    hash = mix(hash, certificate->right.process_id);
    hash = mix(hash, certificate->left_operation_id);
    hash = mix(hash, certificate->right_operation_id);
    hash = mix(hash, certificate->forward_state_count);
    hash = mix(hash, certificate->reverse_state_count);
    hash = mix(hash, certificate->forward_state_fingerprint);
    hash = mix(hash, certificate->reverse_state_fingerprint);
    hash = mix(hash, certificate->program_fingerprint);
    return mix(hash, certificate->prefix_fingerprint);
}

ConcurrencyNativeCommutationStatus concurrency_native_commute(
    const ConcurrencyNativeProgram *program,
    ConcurrencyProcessId left, ConcurrencyProcessId right,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    size_t max_states,
    ConcurrencyNativeCommutationCertificate *certificate) {
    if (!program || !certificate || !max_states ||
        (prefix_count && !prefix) || prefix_count > SIZE_MAX - 2)
        return CONCURRENCY_NATIVE_COMMUTATION_INVALID_ARGUMENT;
    *certificate = (ConcurrencyNativeCommutationCertificate){0};
    size_t total = 0;
    ConcurrencyNativeStatus native_status = validate_program(program, &total);
    if (native_status != CONCURRENCY_NATIVE_CERTIFIED ||
        prefix_count + 2 > total ||
        total > SIZE_MAX / sizeof(ConcurrencyNativeChoice))
        return CONCURRENCY_NATIVE_COMMUTATION_INVALID_ARGUMENT;
    ConcurrencyNativeDependence dependence = {0};
    native_status = concurrency_native_dependence(
        program, left, right, prefix, prefix_count, &dependence);
    if (native_status == CONCURRENCY_NATIVE_OUT_OF_MEMORY)
        return CONCURRENCY_NATIVE_COMMUTATION_OUT_OF_MEMORY;
    if (native_status != CONCURRENCY_NATIVE_CERTIFIED)
        return CONCURRENCY_NATIVE_COMMUTATION_NOT_COENABLED;
    if (dependence.dependent)
        return CONCURRENCY_NATIVE_COMMUTATION_DEPENDENT;

    ConcurrencyNativeFrontier initial = {0};
    native_status = concurrency_native_frontier(
        program, prefix, prefix_count, &initial);
    if (native_status != CONCURRENCY_NATIVE_CERTIFIED)
        return native_status == CONCURRENCY_NATIVE_OUT_OF_MEMORY
            ? CONCURRENCY_NATIVE_COMMUTATION_OUT_OF_MEMORY
            : CONCURRENCY_NATIVE_COMMUTATION_INVALID_ARGUMENT;
    uint64_t left_operation = 0;
    uint64_t right_operation = 0;
    size_t initial_left_local = 0;
    size_t initial_right_local = 0;
    bool coenabled = frontier_process_enabled(
        &initial, left, &left_operation) &&
        frontier_process_enabled(&initial, right, &right_operation);
    for (size_t i = 0; i < initial.process_count; i++) {
        if (process_equal(initial.entries[i].process, left))
            initial_left_local = initial.entries[i].local_index;
        if (process_equal(initial.entries[i].process, right))
            initial_right_local = initial.entries[i].local_index;
    }
    concurrency_native_frontier_free(&initial);
    if (!coenabled) return CONCURRENCY_NATIVE_COMMUTATION_NOT_COENABLED;

    ConcurrencyNativeChoice *forward = calloc(total, sizeof(*forward));
    ConcurrencyNativeChoice *reverse = calloc(total, sizeof(*reverse));
    if (!forward || !reverse) {
        free(forward);
        free(reverse);
        return CONCURRENCY_NATIVE_COMMUTATION_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < prefix_count; i++) {
        forward[i] = prefix[i];
        reverse[i] = prefix[i];
    }
    if (prefix_count && prefix[prefix_count - 1].tick > UINT64_MAX - 2) {
        free(forward);
        free(reverse);
        return CONCURRENCY_NATIVE_COMMUTATION_INVALID_ARGUMENT;
    }
    uint64_t tick = prefix_count ? prefix[prefix_count - 1].tick + 1 : 0;
    forward[prefix_count] = (ConcurrencyNativeChoice){
        .process = left, .local_index = initial_left_local, .tick = tick,
    };
    reverse[prefix_count] = (ConcurrencyNativeChoice){
        .process = right, .local_index = initial_right_local, .tick = tick,
    };
    ConcurrencyNativeFrontier after_first = {0};
    native_status = concurrency_native_frontier(
        program, forward, prefix_count + 1, &after_first);
    if (native_status != CONCURRENCY_NATIVE_CERTIFIED ||
        !frontier_process_enabled(&after_first, right, NULL)) {
        concurrency_native_frontier_free(&after_first);
        free(forward);
        free(reverse);
        return CONCURRENCY_NATIVE_COMMUTATION_DOES_NOT_COMMUTE;
    }
    size_t right_local = 0;
    for (size_t i = 0; i < after_first.process_count; i++)
        if (process_equal(after_first.entries[i].process, right))
            right_local = after_first.entries[i].local_index;
    concurrency_native_frontier_free(&after_first);
    native_status = concurrency_native_frontier(
        program, reverse, prefix_count + 1, &after_first);
    if (native_status != CONCURRENCY_NATIVE_CERTIFIED ||
        !frontier_process_enabled(&after_first, left, NULL)) {
        concurrency_native_frontier_free(&after_first);
        free(forward);
        free(reverse);
        return CONCURRENCY_NATIVE_COMMUTATION_DOES_NOT_COMMUTE;
    }
    size_t left_local = 0;
    for (size_t i = 0; i < after_first.process_count; i++)
        if (process_equal(after_first.entries[i].process, left))
            left_local = after_first.entries[i].local_index;
    concurrency_native_frontier_free(&after_first);
    forward[prefix_count + 1] = (ConcurrencyNativeChoice){
        .process = right, .local_index = right_local, .tick = tick + 1,
    };
    reverse[prefix_count + 1] = (ConcurrencyNativeChoice){
        .process = left, .local_index = left_local, .tick = tick + 1,
    };

    size_t forward_count = 0;
    size_t reverse_count = 0;
    uint64_t forward_fingerprint = 0;
    uint64_t reverse_fingerprint = 0;
    ConcurrencyNativeCommutationStatus status = native_future_fingerprint(
        program, forward, prefix_count + 2, total, max_states,
        &forward_count, &forward_fingerprint);
    if (status == CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED)
        status = native_future_fingerprint(
            program, reverse, prefix_count + 2, total, max_states,
            &reverse_count, &reverse_fingerprint);
    free(forward);
    free(reverse);
    if (status != CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED) return status;
    if (forward_count != reverse_count ||
        forward_fingerprint != reverse_fingerprint)
        return CONCURRENCY_NATIVE_COMMUTATION_DOES_NOT_COMMUTE;
    *certificate = (ConcurrencyNativeCommutationCertificate){
        .prefix_count = prefix_count,
        .max_states = max_states,
        .left = left,
        .right = right,
        .left_operation_id = left_operation,
        .right_operation_id = right_operation,
        .forward_state_count = forward_count,
        .reverse_state_count = reverse_count,
        .forward_state_fingerprint = forward_fingerprint,
        .reverse_state_fingerprint = reverse_fingerprint,
        .program_fingerprint = program_fingerprint(program),
        .prefix_fingerprint = prefix_fingerprint(prefix, prefix_count),
    };
    certificate->fingerprint =
        native_commutation_certificate_fingerprint(certificate);
    return CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED;
}

ConcurrencyNativeCommutationStatus concurrency_native_commutation_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeCommutationCertificate *certificate) {
    if (!program || !certificate || prefix_count != certificate->prefix_count)
        return CONCURRENCY_NATIVE_COMMUTATION_INVALID_ARGUMENT;
    if (certificate->fingerprint !=
        native_commutation_certificate_fingerprint(certificate))
        return CONCURRENCY_NATIVE_COMMUTATION_INVALID_CERTIFICATE;
    ConcurrencyNativeCommutationCertificate expected = {0};
    ConcurrencyNativeCommutationStatus status = concurrency_native_commute(
        program, certificate->left, certificate->right,
        prefix, prefix_count, certificate->max_states, &expected);
    if (status != CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED) return status;
    return expected.fingerprint == certificate->fingerprint
        ? CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED
        : CONCURRENCY_NATIVE_COMMUTATION_INVALID_CERTIFICATE;
}

static uint64_t schedule_fingerprint(const ConcurrencyNativeSchedule *schedule) {
    return prefix_fingerprint(schedule->choices, schedule->choice_count);
}

static uint64_t exploration_fingerprint(
    const ConcurrencyNativeExploration *exploration) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, exploration->mode);
    hash = mix(hash, exploration->limits.max_schedules);
    hash = mix(hash, exploration->limits.max_commutation_states);
    hash = mix(hash, exploration->limits.max_equivalence_states);
    hash = mix(hash, exploration->limits.max_wakeup_obligations);
    hash = mix(hash, exploration->schedule_count);
    hash = mix(hash, exploration->branch_count);
    hash = mix(hash, exploration->dead_end_count);
    hash = mix(hash, exploration->commutation_count);
    hash = mix(hash, exploration->pruned_choice_count);
    hash = mix(hash, exploration->fallback_branch_count);
    hash = mix(hash, exploration->equivalence_class_count);
    hash = mix(hash, exploration->examined_schedule_count);
    hash = mix(hash, exploration->redundant_schedule_count);
    hash = mix(hash, exploration->ordered_insertion_count);
    hash = mix(hash, exploration->suppressed_insertion_count);
    hash = mix(hash, exploration->sleep_transition_count);
    hash = mix(hash, exploration->sleep_blocked_count);
    hash = mix(hash, exploration->wakeup_node_count);
    for (size_t i = 0; i < exploration->wakeup_node_count; i++) {
        const ConcurrencyNativeWakeupNode *node =
            &exploration->wakeup_nodes[i];
        hash = mix(hash, node->parent_index);
        hash = mix(hash, node->choice.process.module_id);
        hash = mix(hash, node->choice.process.process_id);
        hash = mix(hash, node->choice.local_index);
        hash = mix(hash, node->choice.tick);
        hash = mix(hash, node->depth);
        hash = mix(hash, node->terminal);
    }
    hash = mix(hash, exploration->wakeup_obligation_count);
    hash = mix(hash, exploration->discharged_wakeup_obligation_count);
    for (size_t i = 0; i < exploration->wakeup_obligation_count; i++)
        hash = mix(hash, exploration->wakeup_obligations[i].fingerprint);
    hash = mix(hash, exploration->program_fingerprint);
    for (size_t i = 0; i < exploration->schedule_count; i++)
        hash = mix(hash, exploration->schedules[i].schedule_fingerprint);
    return hash;
}

void concurrency_native_exploration_free(
    ConcurrencyNativeExploration *exploration) {
    if (!exploration) return;
    for (size_t i = 0; i < exploration->schedule_count; i++)
        free(exploration->schedules[i].choices);
    free(exploration->schedules);
    free(exploration->wakeup_nodes);
    for (size_t i = 0; i < exploration->wakeup_obligation_count; i++) {
        free(exploration->wakeup_obligations[i].prefix);
        free(exploration->wakeup_obligations[i].sequence);
    }
    free(exploration->wakeup_obligations);
    *exploration = (ConcurrencyNativeExploration){0};
}

typedef struct {
    const ConcurrencyNativeProgram *program;
    size_t total;
    ConcurrencyNativeChoice *prefix;
    ConcurrencyNativeExploration *result;
} Explorer;

static ConcurrencyNativeStatus append_schedule(Explorer *explorer) {
    ConcurrencyNativeExploration *result = explorer->result;
    if (result->schedule_count >= result->limits.max_schedules)
        return CONCURRENCY_NATIVE_LIMIT_EXCEEDED;
    if (result->schedule_count >= SIZE_MAX / sizeof(ConcurrencyNativeSchedule))
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    size_t count = result->schedule_count + 1;
    ConcurrencyNativeSchedule *grown = realloc(
        result->schedules, count * sizeof(*grown));
    if (!grown) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    result->schedules = grown;
    ConcurrencyNativeSchedule *schedule = &grown[count - 1];
    *schedule = (ConcurrencyNativeSchedule){0};
    schedule->choices = malloc(explorer->total * sizeof(*schedule->choices));
    if (!schedule->choices) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    for (size_t i = 0; i < explorer->total; i++)
        schedule->choices[i] = explorer->prefix[i];
    schedule->choice_count = explorer->total;
    schedule->schedule_fingerprint = schedule_fingerprint(schedule);
    result->schedule_count = count;
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static ConcurrencyNativeStatus explore_prefix(Explorer *explorer, size_t depth) {
    if (depth == explorer->total) return append_schedule(explorer);
    ConcurrencyNativeFrontier frontier = {0};
    ConcurrencyNativeStatus status = concurrency_native_frontier(
        explorer->program, explorer->prefix, depth, &frontier);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    if (!frontier.enabled_count) {
        explorer->result->dead_end_count++;
        concurrency_native_frontier_free(&frontier);
        return CONCURRENCY_NATIVE_CERTIFIED;
    }
    bool reduced = explorer->result->mode ==
        CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_REDUCED;
    size_t canonical = SIZE_MAX;
    bool prune = reduced && frontier.enabled_count > 1;
    if (prune) {
        for (size_t i = 0; i < frontier.process_count; i++)
            if (frontier.entries[i].enabled) {
                canonical = i;
                break;
            }
        for (size_t i = canonical + 1;
             i < frontier.process_count && prune; i++) {
            if (!frontier.entries[i].enabled) continue;
            ConcurrencyNativeCommutationCertificate certificate = {0};
            ConcurrencyNativeCommutationStatus commute =
                concurrency_native_commute(
                    explorer->program,
                    frontier.entries[canonical].process,
                    frontier.entries[i].process,
                    explorer->prefix, depth,
                    explorer->result->limits.max_commutation_states,
                    &certificate);
            if (commute == CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED) {
                explorer->result->commutation_count++;
            } else if (commute == CONCURRENCY_NATIVE_COMMUTATION_DEPENDENT ||
                       commute == CONCURRENCY_NATIVE_COMMUTATION_NOT_COENABLED ||
                       commute ==
                           CONCURRENCY_NATIVE_COMMUTATION_DOES_NOT_COMMUTE) {
                prune = false;
                explorer->result->fallback_branch_count++;
            } else if (commute ==
                       CONCURRENCY_NATIVE_COMMUTATION_LIMIT_EXCEEDED) {
                concurrency_native_frontier_free(&frontier);
                return CONCURRENCY_NATIVE_LIMIT_EXCEEDED;
            } else if (commute ==
                       CONCURRENCY_NATIVE_COMMUTATION_OUT_OF_MEMORY) {
                concurrency_native_frontier_free(&frontier);
                return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
            } else {
                concurrency_native_frontier_free(&frontier);
                return CONCURRENCY_NATIVE_INVALID_PROGRAM;
            }
        }
        if (prune)
            explorer->result->pruned_choice_count +=
                frontier.enabled_count - 1;
    }
    size_t explored_count = prune ? 1 : frontier.enabled_count;
    if (explored_count > 1) explorer->result->branch_count++;
    for (size_t i = 0; i < frontier.process_count; i++) {
        if (!frontier.entries[i].enabled) continue;
        if (prune && i != canonical) continue;
        explorer->prefix[depth] = (ConcurrencyNativeChoice){
            .process = frontier.entries[i].process,
            .local_index = frontier.entries[i].local_index,
            .tick = depth,
        };
        status = explore_prefix(explorer, depth + 1);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) break;
    }
    concurrency_native_frontier_free(&frontier);
    return status;
}

static ConcurrencyNativeStatus native_explore_mode(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExplorationMode mode,
    ConcurrencyNativeExploration *exploration) {
    if (!exploration || !limits.max_schedules ||
        (mode != CONCURRENCY_NATIVE_EXPLORATION_EXHAUSTIVE &&
         mode != CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_REDUCED))
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    if (mode == CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_REDUCED &&
        !limits.max_commutation_states)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *exploration = (ConcurrencyNativeExploration){
        .mode = mode, .limits = limits,
    };
    size_t total = 0;
    ConcurrencyNativeStatus status = validate_program(program, &total);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    if (!total || total > SIZE_MAX / sizeof(ConcurrencyNativeChoice))
        return CONCURRENCY_NATIVE_INVALID_PROGRAM;
    ConcurrencyNativeChoice *prefix = calloc(total, sizeof(*prefix));
    if (!prefix) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    exploration->program_fingerprint = program_fingerprint(program);
    Explorer explorer = {
        .program = program, .total = total,
        .prefix = prefix, .result = exploration,
    };
    status = explore_prefix(&explorer, 0);
    free(prefix);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) {
        concurrency_native_exploration_free(exploration);
        return status;
    }
    exploration->fingerprint = exploration_fingerprint(exploration);
    return CONCURRENCY_NATIVE_CERTIFIED;
}

ConcurrencyNativeStatus concurrency_native_explore(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExploration *exploration) {
    return native_explore_mode(
        program, limits, CONCURRENCY_NATIVE_EXPLORATION_EXHAUSTIVE,
        exploration);
}

ConcurrencyNativeStatus concurrency_native_explore_reduced(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExploration *exploration) {
    return native_explore_mode(
        program, limits, CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_REDUCED,
        exploration);
}

static bool exploration_equal(
    const ConcurrencyNativeExploration *left,
    const ConcurrencyNativeExploration *right) {
    if (left->limits.max_schedules != right->limits.max_schedules ||
        left->limits.max_commutation_states !=
            right->limits.max_commutation_states ||
        left->limits.max_equivalence_states !=
            right->limits.max_equivalence_states ||
        left->limits.max_wakeup_obligations !=
            right->limits.max_wakeup_obligations ||
        left->mode != right->mode ||
        left->schedule_count != right->schedule_count ||
        left->branch_count != right->branch_count ||
        left->dead_end_count != right->dead_end_count ||
        left->commutation_count != right->commutation_count ||
        left->pruned_choice_count != right->pruned_choice_count ||
        left->fallback_branch_count != right->fallback_branch_count ||
        left->equivalence_class_count != right->equivalence_class_count ||
        left->examined_schedule_count != right->examined_schedule_count ||
        left->redundant_schedule_count != right->redundant_schedule_count ||
        left->ordered_insertion_count != right->ordered_insertion_count ||
        left->suppressed_insertion_count !=
            right->suppressed_insertion_count ||
        left->sleep_transition_count != right->sleep_transition_count ||
        left->sleep_blocked_count != right->sleep_blocked_count ||
        left->wakeup_node_count != right->wakeup_node_count ||
        left->wakeup_obligation_count != right->wakeup_obligation_count ||
        left->discharged_wakeup_obligation_count !=
            right->discharged_wakeup_obligation_count ||
        left->program_fingerprint != right->program_fingerprint ||
        left->fingerprint != right->fingerprint)
        return false;
    for (size_t i = 0; i < left->wakeup_node_count; i++) {
        const ConcurrencyNativeWakeupNode *a = &left->wakeup_nodes[i];
        const ConcurrencyNativeWakeupNode *b = &right->wakeup_nodes[i];
        if (a->parent_index != b->parent_index ||
            !native_choice_identity_equal(a->choice, b->choice) ||
            a->choice.tick != b->choice.tick || a->depth != b->depth ||
            a->terminal != b->terminal)
            return false;
    }
    for (size_t i = 0; i < left->wakeup_obligation_count; i++) {
        const ConcurrencyNativeWakeupObligation *a =
            &left->wakeup_obligations[i];
        const ConcurrencyNativeWakeupObligation *b =
            &right->wakeup_obligations[i];
        if (a->prefix_count != b->prefix_count ||
            a->sequence_count != b->sequence_count ||
            a->discharged != b->discharged ||
            a->source_schedule_fingerprint !=
                b->source_schedule_fingerprint ||
            a->race_fingerprint != b->race_fingerprint ||
            a->fingerprint != b->fingerprint)
            return false;
        for (size_t j = 0; j < a->prefix_count; j++)
            if (!native_choice_identity_equal(a->prefix[j], b->prefix[j]) ||
                a->prefix[j].tick != b->prefix[j].tick)
                return false;
        for (size_t j = 0; j < a->sequence_count; j++)
            if (!native_choice_identity_equal(
                    a->sequence[j], b->sequence[j]) ||
                a->sequence[j].tick != b->sequence[j].tick)
                return false;
    }
    for (size_t i = 0; i < left->schedule_count; i++) {
        const ConcurrencyNativeSchedule *a = &left->schedules[i];
        const ConcurrencyNativeSchedule *b = &right->schedules[i];
        if (a->choice_count != b->choice_count ||
            a->schedule_fingerprint != b->schedule_fingerprint)
            return false;
        for (size_t j = 0; j < a->choice_count; j++)
            if (!process_equal(a->choices[j].process, b->choices[j].process) ||
                a->choices[j].local_index != b->choices[j].local_index ||
                a->choices[j].tick != b->choices[j].tick)
                return false;
    }
    return true;
}

ConcurrencyNativeStatus concurrency_native_exploration_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeExploration *exploration) {
    if (!program || !exploration || !exploration->limits.max_schedules ||
        (exploration->mode != CONCURRENCY_NATIVE_EXPLORATION_EXHAUSTIVE &&
         exploration->mode !=
             CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_REDUCED &&
         exploration->mode !=
             CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_WAKEUP_TREE &&
         exploration->mode !=
             CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ONLINE_WAKEUP_TREE &&
         exploration->mode !=
             CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_RACE_WAKEUP &&
         exploration->mode !=
             CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ORDERED_WAKEUP) ||
        (exploration->schedule_count && !exploration->schedules) ||
        (exploration->wakeup_node_count && !exploration->wakeup_nodes) ||
        (exploration->wakeup_obligation_count &&
         !exploration->wakeup_obligations))
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    size_t total = 0;
    if (validate_program(program, &total) != CONCURRENCY_NATIVE_CERTIFIED)
        return CONCURRENCY_NATIVE_INVALID_PROGRAM;
    bool wakeup_mode = exploration->mode ==
            CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_WAKEUP_TREE ||
        exploration->mode ==
            CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ONLINE_WAKEUP_TREE ||
        exploration->mode ==
            CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_RACE_WAKEUP ||
        exploration->mode ==
            CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ORDERED_WAKEUP;
    if (wakeup_mode) {
        if (!exploration->wakeup_node_count ||
            exploration->equivalence_class_count !=
                exploration->schedule_count ||
            exploration->schedule_count > (SIZE_MAX - 1) / total ||
            exploration->wakeup_node_count >
                1 + exploration->schedule_count * total)
            return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
        const ConcurrencyNativeWakeupNode *root =
            &exploration->wakeup_nodes[0];
        if (root->parent_index != SIZE_MAX || root->depth != 0 ||
            root->terminal)
            return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
        for (size_t i = 1; i < exploration->wakeup_node_count; i++) {
            const ConcurrencyNativeWakeupNode *node =
                &exploration->wakeup_nodes[i];
            if (node->parent_index >= i || node->depth !=
                    exploration->wakeup_nodes[node->parent_index].depth + 1)
                return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
        }
        if (exploration->mode ==
                CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ONLINE_WAKEUP_TREE) {
            if (!exploration->limits.max_equivalence_states ||
                exploration->wakeup_obligation_count ||
                exploration->wakeup_obligations ||
                exploration->discharged_wakeup_obligation_count ||
                exploration->examined_schedule_count <
                    exploration->schedule_count ||
                exploration->redundant_schedule_count !=
                    exploration->examined_schedule_count -
                        exploration->schedule_count)
                return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
        } else if (exploration->mode ==
                       CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_RACE_WAKEUP ||
                   exploration->mode ==
                       CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ORDERED_WAKEUP) {
            if (!exploration->limits.max_equivalence_states ||
                !exploration->limits.max_wakeup_obligations ||
                exploration->wakeup_obligation_count >
                    exploration->limits.max_wakeup_obligations ||
                exploration->discharged_wakeup_obligation_count !=
                    exploration->wakeup_obligation_count ||
                exploration->examined_schedule_count <
                    exploration->schedule_count ||
                exploration->redundant_schedule_count !=
                    exploration->examined_schedule_count -
                        exploration->schedule_count)
                return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
            if (exploration->mode ==
                    CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ORDERED_WAKEUP &&
                (exploration->ordered_insertion_count !=
                     exploration->wakeup_obligation_count ||
                 exploration->suppressed_insertion_count >
                     exploration->ordered_insertion_count ||
                 exploration->sleep_blocked_count))
                return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
            for (size_t i = 0;
                 i < exploration->wakeup_obligation_count; i++) {
                const ConcurrencyNativeWakeupObligation *obligation =
                    &exploration->wakeup_obligations[i];
                if (!obligation->discharged || !obligation->sequence_count ||
                    (obligation->prefix_count && !obligation->prefix) ||
                    !obligation->sequence ||
                    obligation->prefix_count > total ||
                    obligation->sequence_count >
                        total - obligation->prefix_count ||
                    obligation->fingerprint !=
                        native_wakeup_obligation_fingerprint(obligation))
                    return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
            }
        } else if (exploration->examined_schedule_count ||
                   exploration->redundant_schedule_count ||
                   exploration->wakeup_obligation_count ||
                   exploration->wakeup_obligations ||
                   exploration->discharged_wakeup_obligation_count)
            return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    } else if (exploration->wakeup_node_count ||
               exploration->wakeup_nodes ||
               exploration->equivalence_class_count ||
               exploration->examined_schedule_count ||
               exploration->redundant_schedule_count)
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    for (size_t i = 0; i < exploration->schedule_count; i++)
        if (exploration->schedules[i].choice_count != total ||
            !exploration->schedules[i].choices ||
            exploration->schedules[i].schedule_fingerprint !=
                schedule_fingerprint(&exploration->schedules[i]))
            return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    if (exploration->fingerprint != exploration_fingerprint(exploration))
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    ConcurrencyNativeExploration expected = {0};
    ConcurrencyNativeStatus status = wakeup_mode
        ? (exploration->mode ==
                   CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ORDERED_WAKEUP
               ? concurrency_native_explore_ordered_wakeup(
                     program, exploration->limits, &expected)
               : exploration->mode ==
                   CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_RACE_WAKEUP
               ? concurrency_native_explore_race_wakeup(
                     program, exploration->limits, &expected)
               : exploration->mode ==
                   CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ONLINE_WAKEUP_TREE
               ? concurrency_native_explore_wakeup_tree_online(
                     program, exploration->limits, &expected)
               : concurrency_native_explore_wakeup_tree(
                     program, exploration->limits, &expected))
        : native_explore_mode(
              program, exploration->limits, exploration->mode, &expected);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    bool equal = exploration_equal(exploration, &expected);
    concurrency_native_exploration_free(&expected);
    return equal ? CONCURRENCY_NATIVE_CERTIFIED
                 : CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
}

static bool native_choice_identity_equal(
    ConcurrencyNativeChoice left, ConcurrencyNativeChoice right) {
    return process_equal(left.process, right.process) &&
           left.local_index == right.local_index;
}

static bool native_schedule_equal(
    const ConcurrencyNativeSchedule *left,
    const ConcurrencyNativeSchedule *right) {
    if (left->choice_count != right->choice_count) return false;
    for (size_t i = 0; i < left->choice_count; i++)
        if (!native_choice_identity_equal(
                left->choices[i], right->choices[i]))
            return false;
    return true;
}

static bool native_swap_neighbor(
    const ConcurrencyNativeSchedule *from,
    const ConcurrencyNativeSchedule *to, size_t position) {
    if (from->choice_count != to->choice_count ||
        position + 1 >= from->choice_count)
        return false;
    for (size_t i = 0; i < from->choice_count; i++) {
        size_t source = i == position ? position + 1
                      : i == position + 1 ? position : i;
        if (!native_choice_identity_equal(
                from->choices[source], to->choices[i]))
            return false;
    }
    return true;
}

static ConcurrencyNativeStatus native_append_schedule_copy(
    ConcurrencyNativeExploration *result,
    const ConcurrencyNativeSchedule *source) {
    if (result->schedule_count >= result->limits.max_schedules)
        return CONCURRENCY_NATIVE_LIMIT_EXCEEDED;
    size_t count = result->schedule_count + 1;
    if (count > SIZE_MAX / sizeof(*result->schedules) ||
        source->choice_count > SIZE_MAX / sizeof(*source->choices))
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    ConcurrencyNativeSchedule *grown = realloc(
        result->schedules, count * sizeof(*grown));
    if (!grown) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    result->schedules = grown;
    ConcurrencyNativeSchedule *copy = &grown[count - 1];
    *copy = (ConcurrencyNativeSchedule){0};
    copy->choices = malloc(source->choice_count * sizeof(*copy->choices));
    if (!copy->choices) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    memcpy(copy->choices, source->choices,
           source->choice_count * sizeof(*copy->choices));
    copy->choice_count = source->choice_count;
    copy->schedule_fingerprint = source->schedule_fingerprint;
    result->schedule_count = count;
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static ConcurrencyNativeStatus native_append_wakeup_node(
    ConcurrencyNativeExploration *exploration, size_t parent_index,
    ConcurrencyNativeChoice choice, size_t depth, size_t *node_index) {
    size_t count = exploration->wakeup_node_count + 1;
    if (count > SIZE_MAX / sizeof(*exploration->wakeup_nodes))
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    ConcurrencyNativeWakeupNode *grown = realloc(
        exploration->wakeup_nodes, count * sizeof(*grown));
    if (!grown) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    exploration->wakeup_nodes = grown;
    grown[count - 1] = (ConcurrencyNativeWakeupNode){
        .parent_index = parent_index,
        .choice = choice,
        .depth = depth,
    };
    exploration->wakeup_node_count = count;
    *node_index = count - 1;
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static ConcurrencyNativeStatus native_insert_wakeup_schedule(
    ConcurrencyNativeExploration *exploration,
    const ConcurrencyNativeSchedule *schedule);

static ConcurrencyNativeStatus native_build_wakeup_trie(
    ConcurrencyNativeExploration *exploration) {
    size_t root = 0;
    ConcurrencyNativeStatus status = native_append_wakeup_node(
        exploration, SIZE_MAX, (ConcurrencyNativeChoice){0}, 0, &root);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    for (size_t i = 0; i < exploration->schedule_count; i++) {
        status = native_insert_wakeup_schedule(
            exploration, &exploration->schedules[i]);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    }
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static ConcurrencyNativeStatus native_insert_wakeup_schedule(
    ConcurrencyNativeExploration *exploration,
    const ConcurrencyNativeSchedule *schedule) {
    size_t parent = 0;
    for (size_t step = 0; step < schedule->choice_count; step++) {
        size_t child = SIZE_MAX;
        for (size_t node = 1; node < exploration->wakeup_node_count; node++)
            if (exploration->wakeup_nodes[node].parent_index == parent &&
                native_choice_identity_equal(
                    exploration->wakeup_nodes[node].choice,
                    schedule->choices[step])) {
                child = node;
                break;
            }
        if (child == SIZE_MAX) {
            ConcurrencyNativeStatus status = native_append_wakeup_node(
                exploration, parent, schedule->choices[step], step + 1,
                &child);
            if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
        }
        parent = child;
    }
    exploration->wakeup_nodes[parent].terminal = true;
    return CONCURRENCY_NATIVE_CERTIFIED;
}

ConcurrencyNativeStatus concurrency_native_explore_wakeup_tree(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExploration *exploration) {
    if (!program || !exploration || !limits.max_schedules ||
        !limits.max_commutation_states)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *exploration = (ConcurrencyNativeExploration){
        .mode = CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_WAKEUP_TREE,
        .limits = limits,
    };
    ConcurrencyNativeExploration exhaustive = {0};
    ConcurrencyNativeStatus status = concurrency_native_explore(
        program, limits, &exhaustive);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    size_t count = exhaustive.schedule_count;
    bool *assigned = count ? calloc(count, sizeof(*assigned)) : NULL;
    bool *queued = count ? calloc(count, sizeof(*queued)) : NULL;
    size_t *queue = count ? malloc(count * sizeof(*queue)) : NULL;
    if (count && (!assigned || !queued || !queue)) {
        status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
        goto done;
    }
    exploration->program_fingerprint = exhaustive.program_fingerprint;
    exploration->dead_end_count = exhaustive.dead_end_count;
    for (size_t seed = 0; seed < count; seed++) {
        if (assigned[seed]) continue;
        memset(queued, 0, count * sizeof(*queued));
        size_t head = 0, tail = 0;
        queue[tail++] = seed;
        queued[seed] = true;
        size_t canonical = seed;
        while (head < tail) {
            size_t current = queue[head++];
            assigned[current] = true;
            if (current < canonical) canonical = current;
            const ConcurrencyNativeSchedule *schedule =
                &exhaustive.schedules[current];
            for (size_t position = 0;
                 position + 1 < schedule->choice_count; position++) {
                ConcurrencyNativeCommutationCertificate certificate = {0};
                ConcurrencyNativeCommutationStatus commute =
                    concurrency_native_commute(
                        program, schedule->choices[position].process,
                        schedule->choices[position + 1].process,
                        schedule->choices, position,
                        limits.max_commutation_states, &certificate);
                if (commute == CONCURRENCY_NATIVE_COMMUTATION_LIMIT_EXCEEDED) {
                    status = CONCURRENCY_NATIVE_LIMIT_EXCEEDED;
                    goto done;
                }
                if (commute == CONCURRENCY_NATIVE_COMMUTATION_OUT_OF_MEMORY) {
                    status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
                    goto done;
                }
                if (commute != CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED)
                    continue;
                exploration->commutation_count++;
                for (size_t next = 0; next < count; next++)
                    if (!queued[next] && native_swap_neighbor(
                            schedule, &exhaustive.schedules[next], position)) {
                        queued[next] = true;
                        queue[tail++] = next;
                        break;
                    }
            }
        }
        status = native_append_schedule_copy(
            exploration, &exhaustive.schedules[canonical]);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) goto done;
        exploration->equivalence_class_count++;
    }
    exploration->pruned_choice_count =
        exhaustive.schedule_count - exploration->schedule_count;
    status = native_build_wakeup_trie(exploration);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) goto done;
    exploration->fingerprint = exploration_fingerprint(exploration);
done:
    free(assigned);
    free(queued);
    free(queue);
    concurrency_native_exploration_free(&exhaustive);
    if (status != CONCURRENCY_NATIVE_CERTIFIED)
        concurrency_native_exploration_free(exploration);
    return status;
}

static void native_schedule_array_free(
    ConcurrencyNativeSchedule *schedules, size_t count) {
    for (size_t i = 0; i < count; i++) free(schedules[i].choices);
    free(schedules);
}

static ConcurrencyNativeStatus native_schedule_array_append(
    ConcurrencyNativeSchedule **schedules, size_t *count,
    const ConcurrencyNativeSchedule *source) {
    size_t grown_count = *count + 1;
    if (grown_count > SIZE_MAX / sizeof(**schedules) ||
        source->choice_count > SIZE_MAX / sizeof(*source->choices))
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    ConcurrencyNativeSchedule *grown = realloc(
        *schedules, grown_count * sizeof(*grown));
    if (!grown) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    *schedules = grown;
    ConcurrencyNativeSchedule *copy = &grown[grown_count - 1];
    *copy = (ConcurrencyNativeSchedule){0};
    copy->choices = malloc(source->choice_count * sizeof(*copy->choices));
    if (!copy->choices) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    memcpy(copy->choices, source->choices,
           source->choice_count * sizeof(*copy->choices));
    copy->choice_count = source->choice_count;
    copy->schedule_fingerprint = source->schedule_fingerprint;
    *count = grown_count;
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static ConcurrencyNativeStatus native_online_schedule_redundant(
    const ConcurrencyNativeProgram *program,
    ConcurrencyNativeExploration *exploration,
    const ConcurrencyNativeSchedule *candidate, bool *redundant) {
    *redundant = false;
    if (!exploration->schedule_count) return CONCURRENCY_NATIVE_CERTIFIED;
    ConcurrencyNativeSchedule *states = NULL;
    size_t state_count = 0;
    ConcurrencyNativeStatus status = native_schedule_array_append(
        &states, &state_count, candidate);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    for (size_t current = 0; current < state_count; current++) {
        const ConcurrencyNativeSchedule *schedule = &states[current];
        for (size_t representative = 0;
             representative < exploration->schedule_count; representative++)
            if (native_schedule_equal(
                    schedule, &exploration->schedules[representative])) {
                *redundant = true;
                goto done;
            }
        for (size_t position = 0;
             position + 1 < states[current].choice_count; position++) {
            schedule = &states[current];
            ConcurrencyNativeCommutationCertificate certificate = {0};
            ConcurrencyNativeCommutationStatus commute =
                concurrency_native_commute(
                    program, schedule->choices[position].process,
                    schedule->choices[position + 1].process,
                    schedule->choices, position,
                    exploration->limits.max_commutation_states,
                    &certificate);
            if (commute == CONCURRENCY_NATIVE_COMMUTATION_LIMIT_EXCEEDED) {
                status = CONCURRENCY_NATIVE_LIMIT_EXCEEDED;
                goto done;
            }
            if (commute == CONCURRENCY_NATIVE_COMMUTATION_OUT_OF_MEMORY) {
                status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
                goto done;
            }
            if (commute != CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED)
                continue;
            exploration->commutation_count++;
            ConcurrencyNativeSchedule source_copy = {
                .choice_count = schedule->choice_count,
                .schedule_fingerprint = schedule->schedule_fingerprint,
            };
            source_copy.choices = malloc(
                source_copy.choice_count * sizeof(*source_copy.choices));
            if (!source_copy.choices) {
                status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
                goto done;
            }
            memcpy(source_copy.choices, schedule->choices,
                   source_copy.choice_count * sizeof(*source_copy.choices));
            status = native_schedule_array_append(
                &states, &state_count, &source_copy);
            free(source_copy.choices);
            if (status != CONCURRENCY_NATIVE_CERTIFIED) goto done;
            ConcurrencyNativeSchedule neighbor = states[state_count - 1];
            ConcurrencyNativeChoice swap = neighbor.choices[position];
            neighbor.choices[position] = neighbor.choices[position + 1];
            neighbor.choices[position + 1] = swap;
            neighbor.choices[position].tick = position;
            neighbor.choices[position + 1].tick = position + 1;
            neighbor.schedule_fingerprint = schedule_fingerprint(&neighbor);
            states[state_count - 1] = neighbor;
            bool seen = false;
            for (size_t prior = 0; prior + 1 < state_count; prior++)
                if (native_schedule_equal(&states[prior], &neighbor)) {
                    seen = true;
                    break;
                }
            if (seen) {
                free(states[state_count - 1].choices);
                state_count--;
            } else if (state_count >
                       exploration->limits.max_equivalence_states) {
                status = CONCURRENCY_NATIVE_LIMIT_EXCEEDED;
                goto done;
            }
        }
    }
done:
    native_schedule_array_free(states, state_count);
    return status;
}

typedef struct {
    const ConcurrencyNativeProgram *program;
    size_t total;
    ConcurrencyNativeChoice *prefix;
    ConcurrencyNativeExploration *result;
} OnlineWakeupExplorer;

static ConcurrencyNativeStatus native_online_wakeup_leaf(
    OnlineWakeupExplorer *explorer) {
    ConcurrencyNativeExploration *result = explorer->result;
    result->examined_schedule_count++;
    ConcurrencyNativeSchedule candidate = {
        .choices = explorer->prefix,
        .choice_count = explorer->total,
    };
    candidate.schedule_fingerprint = schedule_fingerprint(&candidate);
    bool redundant = false;
    ConcurrencyNativeStatus status = native_online_schedule_redundant(
        explorer->program, result, &candidate, &redundant);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    if (redundant) {
        result->redundant_schedule_count++;
        return CONCURRENCY_NATIVE_CERTIFIED;
    }
    status = native_append_schedule_copy(result, &candidate);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    result->equivalence_class_count++;
    return native_insert_wakeup_schedule(
        result, &result->schedules[result->schedule_count - 1]);
}

static ConcurrencyNativeStatus native_online_wakeup_prefix(
    OnlineWakeupExplorer *explorer, size_t depth) {
    if (depth == explorer->total) return native_online_wakeup_leaf(explorer);
    ConcurrencyNativeFrontier frontier = {0};
    ConcurrencyNativeStatus status = concurrency_native_frontier(
        explorer->program, explorer->prefix, depth, &frontier);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    if (!frontier.enabled_count) {
        explorer->result->dead_end_count++;
        concurrency_native_frontier_free(&frontier);
        return CONCURRENCY_NATIVE_CERTIFIED;
    }
    if (frontier.enabled_count > 1) explorer->result->branch_count++;
    for (size_t i = 0; i < frontier.process_count; i++) {
        if (!frontier.entries[i].enabled) continue;
        explorer->prefix[depth] = (ConcurrencyNativeChoice){
            .process = frontier.entries[i].process,
            .local_index = frontier.entries[i].local_index,
            .tick = depth,
        };
        status = native_online_wakeup_prefix(explorer, depth + 1);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) break;
    }
    concurrency_native_frontier_free(&frontier);
    return status;
}

ConcurrencyNativeStatus concurrency_native_explore_wakeup_tree_online(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExploration *exploration) {
    if (!program || !exploration || !limits.max_schedules ||
        !limits.max_commutation_states || !limits.max_equivalence_states)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *exploration = (ConcurrencyNativeExploration){
        .mode =
            CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ONLINE_WAKEUP_TREE,
        .limits = limits,
    };
    size_t total = 0;
    ConcurrencyNativeStatus status = validate_program(program, &total);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    if (!total || total > SIZE_MAX / sizeof(ConcurrencyNativeChoice))
        return CONCURRENCY_NATIVE_INVALID_PROGRAM;
    ConcurrencyNativeChoice *prefix = calloc(total, sizeof(*prefix));
    if (!prefix) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    exploration->program_fingerprint = program_fingerprint(program);
    status = native_build_wakeup_trie(exploration);
    if (status == CONCURRENCY_NATIVE_CERTIFIED) {
        OnlineWakeupExplorer explorer = {
            .program = program, .total = total,
            .prefix = prefix, .result = exploration,
        };
        status = native_online_wakeup_prefix(&explorer, 0);
    }
    free(prefix);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) {
        concurrency_native_exploration_free(exploration);
        return status;
    }
    exploration->pruned_choice_count = exploration->redundant_schedule_count;
    exploration->fingerprint = exploration_fingerprint(exploration);
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static uint64_t native_wakeup_obligation_fingerprint(
    const ConcurrencyNativeWakeupObligation *obligation) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, obligation->prefix_count);
    hash = mix(hash, obligation->sequence_count);
    hash = mix(hash, obligation->discharged);
    hash = mix(hash, obligation->source_schedule_fingerprint);
    hash = mix(hash, obligation->race_fingerprint);
    for (size_t i = 0; i < obligation->prefix_count; i++) {
        hash = mix(hash, obligation->prefix[i].process.module_id);
        hash = mix(hash, obligation->prefix[i].process.process_id);
        hash = mix(hash, obligation->prefix[i].local_index);
        hash = mix(hash, obligation->prefix[i].tick);
    }
    for (size_t i = 0; i < obligation->sequence_count; i++) {
        hash = mix(hash, obligation->sequence[i].process.module_id);
        hash = mix(hash, obligation->sequence[i].process.process_id);
        hash = mix(hash, obligation->sequence[i].local_index);
        hash = mix(hash, obligation->sequence[i].tick);
    }
    return hash;
}

static bool native_plan_matches_schedule(
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *sequence, size_t sequence_count,
    const ConcurrencyNativeSchedule *schedule) {
    if (prefix_count + sequence_count > schedule->choice_count) return false;
    for (size_t i = 0; i < prefix_count; i++)
        if (!native_choice_identity_equal(prefix[i], schedule->choices[i]))
            return false;
    for (size_t i = 0; i < sequence_count; i++)
        if (!native_choice_identity_equal(
                sequence[i], schedule->choices[prefix_count + i]))
            return false;
    return true;
}

static bool native_wakeup_plan_known(
    const ConcurrencyNativeExploration *exploration,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *sequence, size_t sequence_count) {
    for (size_t i = 0; i < exploration->schedule_count; i++)
        if (native_plan_matches_schedule(
                prefix, prefix_count, sequence, sequence_count,
                &exploration->schedules[i]))
            return true;
    for (size_t i = 0; i < exploration->wakeup_obligation_count; i++) {
        const ConcurrencyNativeWakeupObligation *known =
            &exploration->wakeup_obligations[i];
        if (known->prefix_count != prefix_count ||
            known->sequence_count != sequence_count)
            continue;
        bool equal = true;
        for (size_t j = 0; j < prefix_count && equal; j++)
            equal = native_choice_identity_equal(known->prefix[j], prefix[j]);
        for (size_t j = 0; j < sequence_count && equal; j++)
            equal = native_choice_identity_equal(
                known->sequence[j], sequence[j]);
        if (equal) return true;
    }
    return false;
}

static ConcurrencyNativeStatus native_append_wakeup_obligation(
    ConcurrencyNativeExploration *exploration,
    const ConcurrencyNativeSchedule *source,
    const ConcurrencyNativeRace *race) {
    if (native_wakeup_plan_known(
            exploration, source->choices, race->reversal_prefix_count,
            race->reversal, race->reversal_count))
        return CONCURRENCY_NATIVE_CERTIFIED;
    if (exploration->wakeup_obligation_count >=
        exploration->limits.max_wakeup_obligations)
        return CONCURRENCY_NATIVE_LIMIT_EXCEEDED;
    size_t count = exploration->wakeup_obligation_count + 1;
    if (count > SIZE_MAX / sizeof(*exploration->wakeup_obligations))
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    ConcurrencyNativeWakeupObligation *grown = realloc(
        exploration->wakeup_obligations, count * sizeof(*grown));
    if (!grown) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    exploration->wakeup_obligations = grown;
    ConcurrencyNativeWakeupObligation *obligation = &grown[count - 1];
    *obligation = (ConcurrencyNativeWakeupObligation){
        .prefix_count = race->reversal_prefix_count,
        .sequence_count = race->reversal_count,
        .source_schedule_fingerprint = source->schedule_fingerprint,
        .race_fingerprint = race->fingerprint,
    };
    if (obligation->prefix_count) {
        obligation->prefix = malloc(
            obligation->prefix_count * sizeof(*obligation->prefix));
        if (!obligation->prefix) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
        memcpy(obligation->prefix, source->choices,
               obligation->prefix_count * sizeof(*obligation->prefix));
    }
    obligation->sequence = malloc(
        obligation->sequence_count * sizeof(*obligation->sequence));
    if (!obligation->sequence) {
        free(obligation->prefix);
        obligation->prefix = NULL;
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    }
    memcpy(obligation->sequence, race->reversal,
           obligation->sequence_count * sizeof(*obligation->sequence));
    obligation->fingerprint =
        native_wakeup_obligation_fingerprint(obligation);
    exploration->wakeup_obligation_count = count;
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static ConcurrencyNativeStatus native_force_choice(
    const ConcurrencyNativeProgram *program,
    ConcurrencyNativeExploration *exploration,
    ConcurrencyNativeChoice *schedule, size_t depth,
    ConcurrencyNativeChoice forced) {
    ConcurrencyNativeFrontier frontier = {0};
    ConcurrencyNativeStatus status = concurrency_native_frontier(
        program, schedule, depth, &frontier);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    if (frontier.enabled_count > 1) exploration->branch_count++;
    bool found = false;
    for (size_t i = 0; i < frontier.process_count; i++)
        if (frontier.entries[i].enabled &&
            process_equal(frontier.entries[i].process, forced.process) &&
            frontier.entries[i].local_index == forced.local_index) {
            schedule[depth] = (ConcurrencyNativeChoice){
                .process = forced.process,
                .local_index = forced.local_index,
                .tick = depth,
            };
            found = true;
            break;
        }
    concurrency_native_frontier_free(&frontier);
    return found ? CONCURRENCY_NATIVE_CERTIFIED
                 : CONCURRENCY_NATIVE_MALFORMED_PREFIX;
}

static ConcurrencyNativeStatus native_build_race_wakeup_execution(
    const ConcurrencyNativeProgram *program,
    ConcurrencyNativeExploration *exploration, size_t total,
    const ConcurrencyNativeWakeupObligation *obligation,
    ConcurrencyNativeSchedule *schedule) {
    *schedule = (ConcurrencyNativeSchedule){0};
    schedule->choices = calloc(total, sizeof(*schedule->choices));
    if (!schedule->choices) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    size_t depth = 0;
    if (obligation) {
        for (size_t i = 0; i < obligation->prefix_count; i++, depth++) {
            ConcurrencyNativeStatus status = native_force_choice(
                program, exploration, schedule->choices, depth,
                obligation->prefix[i]);
            if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
        }
        for (size_t i = 0; i < obligation->sequence_count; i++, depth++) {
            ConcurrencyNativeStatus status = native_force_choice(
                program, exploration, schedule->choices, depth,
                obligation->sequence[i]);
            if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
        }
    }
    while (depth < total) {
        ConcurrencyNativeFrontier frontier = {0};
        ConcurrencyNativeStatus status = concurrency_native_frontier(
            program, schedule->choices, depth, &frontier);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
        if (!frontier.enabled_count) {
            exploration->dead_end_count++;
            concurrency_native_frontier_free(&frontier);
            return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
        }
        if (frontier.enabled_count > 1) exploration->branch_count++;
        for (size_t i = 0; i < frontier.process_count; i++)
            if (frontier.entries[i].enabled) {
                schedule->choices[depth] = (ConcurrencyNativeChoice){
                    .process = frontier.entries[i].process,
                    .local_index = frontier.entries[i].local_index,
                    .tick = depth,
                };
                break;
            }
        concurrency_native_frontier_free(&frontier);
        depth++;
    }
    schedule->choice_count = total;
    schedule->schedule_fingerprint = schedule_fingerprint(schedule);
    return CONCURRENCY_NATIVE_CERTIFIED;
}

static ConcurrencyNativeStatus native_process_race_wakeup_execution(
    const ConcurrencyNativeProgram *program,
    ConcurrencyNativeExploration *exploration,
    const ConcurrencyNativeSchedule *candidate) {
    exploration->examined_schedule_count++;
    bool redundant = false;
    ConcurrencyNativeStatus status = native_online_schedule_redundant(
        program, exploration, candidate, &redundant);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    if (redundant) {
        exploration->redundant_schedule_count++;
        return CONCURRENCY_NATIVE_CERTIFIED;
    }
    status = native_append_schedule_copy(exploration, candidate);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    exploration->equivalence_class_count++;
    status = native_insert_wakeup_schedule(
        exploration,
        &exploration->schedules[exploration->schedule_count - 1]);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    ConcurrencyNativeRaceCertificate races = {0};
    status = concurrency_native_races(
        program, candidate->choices, candidate->choice_count, &races);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    for (size_t i = 0; i < races.race_count; i++) {
        status = native_append_wakeup_obligation(
            exploration, candidate, &races.races[i]);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) break;
    }
    concurrency_native_races_free(&races);
    return status;
}

ConcurrencyNativeStatus concurrency_native_explore_race_wakeup(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExploration *exploration) {
    if (!program || !exploration || !limits.max_schedules ||
        !limits.max_commutation_states || !limits.max_equivalence_states ||
        !limits.max_wakeup_obligations)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *exploration = (ConcurrencyNativeExploration){
        .mode = CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_RACE_WAKEUP,
        .limits = limits,
    };
    size_t total = 0;
    ConcurrencyNativeStatus status = validate_program(program, &total);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    exploration->program_fingerprint = program_fingerprint(program);
    status = native_build_wakeup_trie(exploration);
    ConcurrencyNativeSchedule candidate = {0};
    if (status == CONCURRENCY_NATIVE_CERTIFIED)
        status = native_build_race_wakeup_execution(
            program, exploration, total, NULL, &candidate);
    if (status == CONCURRENCY_NATIVE_CERTIFIED)
        status = native_process_race_wakeup_execution(
            program, exploration, &candidate);
    free(candidate.choices);
    candidate = (ConcurrencyNativeSchedule){0};
    for (size_t cursor = 0;
         status == CONCURRENCY_NATIVE_CERTIFIED &&
         cursor < exploration->wakeup_obligation_count; cursor++) {
        ConcurrencyNativeWakeupObligation *obligation =
            &exploration->wakeup_obligations[cursor];
        status = native_build_race_wakeup_execution(
            program, exploration, total, obligation, &candidate);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) break;
        obligation = &exploration->wakeup_obligations[cursor];
        obligation->discharged = true;
        obligation->fingerprint =
            native_wakeup_obligation_fingerprint(obligation);
        exploration->discharged_wakeup_obligation_count++;
        status = native_process_race_wakeup_execution(
            program, exploration, &candidate);
        free(candidate.choices);
        candidate = (ConcurrencyNativeSchedule){0};
    }
    free(candidate.choices);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) {
        concurrency_native_exploration_free(exploration);
        return status;
    }
    exploration->fingerprint = exploration_fingerprint(exploration);
    return CONCURRENCY_NATIVE_CERTIFIED;
}

ConcurrencyNativeStatus concurrency_native_explore_ordered_wakeup(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExploration *exploration) {
    ConcurrencyNativeStatus status = concurrency_native_explore_race_wakeup(
        program, limits, exploration);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    exploration->mode =
        CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ORDERED_WAKEUP;
    ConcurrencyNativeLimits proof_limits = limits;
    if (proof_limits.max_schedules < proof_limits.max_equivalence_states)
        proof_limits.max_schedules = proof_limits.max_equivalence_states;
    for (size_t obligation_index = 0;
         obligation_index < exploration->wakeup_obligation_count;
         obligation_index++) {
        const ConcurrencyNativeWakeupObligation *obligation =
            &exploration->wakeup_obligations[obligation_index];
        size_t source_index = SIZE_MAX;
        for (size_t i = 0; i < exploration->schedule_count; i++)
            if (exploration->schedules[i].schedule_fingerprint ==
                    obligation->source_schedule_fingerprint) {
                source_index = i;
                break;
            }
        if (source_index == SIZE_MAX) {
            status = CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
            goto fail;
        }
        ConcurrencyProcessId *sleep = calloc(
            source_index + 1, sizeof(*sleep));
        if (!sleep) {
            status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
            goto fail;
        }
        size_t sleep_count = 0;
        for (size_t i = 0; i <= source_index; i++) {
            const ConcurrencyNativeSchedule *schedule =
                &exploration->schedules[i];
            if (obligation->prefix_count >= schedule->choice_count ||
                !native_plan_matches_schedule(
                    obligation->prefix, obligation->prefix_count,
                    NULL, 0, schedule))
                continue;
            ConcurrencyProcessId process =
                schedule->choices[obligation->prefix_count].process;
            bool known = false;
            for (size_t j = 0; j < sleep_count; j++)
                if (process_equal(sleep[j], process)) known = true;
            if (!known) sleep[sleep_count++] = process;
        }
        ConcurrencyNativeWakeupInsertCertificate insertion = {0};
        status = concurrency_native_wakeup_insert(
            program, obligation->prefix, obligation->prefix_count,
            sleep, sleep_count, NULL, 0,
            obligation->sequence, obligation->sequence_count,
            proof_limits, &insertion);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) {
            free(sleep);
            goto fail;
        }
        exploration->ordered_insertion_count++;
        if (insertion.suppressed)
            exploration->suppressed_insertion_count++;
        concurrency_native_wakeup_insert_free(&insertion);
        if (obligation->prefix_count > SIZE_MAX - obligation->sequence_count) {
            free(sleep);
            status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
            goto fail;
        }
        size_t combined_count =
            obligation->prefix_count + obligation->sequence_count;
        ConcurrencyNativeChoice *combined = malloc(
            combined_count * sizeof(*combined));
        if (!combined) {
            free(sleep);
            status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
            goto fail;
        }
        if (obligation->prefix_count)
            memcpy(combined, obligation->prefix,
                   obligation->prefix_count * sizeof(*combined));
        for (size_t step_index = 0;
             step_index < obligation->sequence_count; step_index++) {
            ConcurrencyProcessId selected =
                obligation->sequence[step_index].process;
            bool blocked = false;
            for (size_t i = 0; i < sleep_count; i++)
                if (process_equal(sleep[i], selected)) blocked = true;
            if (blocked) {
                exploration->sleep_blocked_count++;
                status = CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
                break;
            }
            ConcurrencyNativeSleepStepCertificate transfer = {0};
            status = concurrency_native_sleep_step(
                program, combined,
                obligation->prefix_count + step_index,
                sleep, sleep_count, selected, &transfer);
            if (status != CONCURRENCY_NATIVE_CERTIFIED) break;
            free(sleep);
            sleep = transfer.retained;
            sleep_count = transfer.retained_count;
            transfer.retained = NULL;
            transfer.retained_count = 0;
            concurrency_native_sleep_step_free(&transfer);
            combined[obligation->prefix_count + step_index] =
                obligation->sequence[step_index];
            exploration->sleep_transition_count++;
        }
        free(combined);
        free(sleep);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) goto fail;
    }
    exploration->fingerprint = exploration_fingerprint(exploration);
    return CONCURRENCY_NATIVE_CERTIFIED;
fail:
    concurrency_native_exploration_free(exploration);
    return status;
}

static uint64_t native_weak_initial_fingerprint(
    const ConcurrencyNativeWeakInitialCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->limits.max_schedules);
    hash = mix(hash, certificate->limits.max_commutation_states);
    hash = mix(hash, certificate->limits.max_equivalence_states);
    hash = mix(hash, certificate->limits.max_wakeup_obligations);
    hash = mix(hash, certificate->prefix_count);
    hash = mix(hash, certificate->left_count);
    hash = mix(hash, certificate->right_count);
    hash = mix(hash, certificate->related);
    hash = mix(hash, certificate->left_schedule_index);
    hash = mix(hash, certificate->right_schedule_index);
    hash = mix(hash, certificate->swap_count);
    hash = mix(hash, certificate->program_fingerprint);
    hash = mix(hash, certificate->prefix_fingerprint);
    hash = mix(hash, certificate->left_fingerprint);
    hash = mix(hash, certificate->right_fingerprint);
    for (size_t i = 0; i < certificate->swap_count; i++)
        hash = mix(hash, certificate->swap_positions[i]);
    return hash;
}

void concurrency_native_weak_initial_free(
    ConcurrencyNativeWeakInitialCertificate *certificate) {
    if (!certificate) return;
    free(certificate->swap_positions);
    *certificate = (ConcurrencyNativeWeakInitialCertificate){0};
}

ConcurrencyNativeStatus concurrency_native_weak_initial(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *left, size_t left_count,
    const ConcurrencyNativeChoice *right, size_t right_count,
    ConcurrencyNativeLimits limits,
    ConcurrencyNativeWeakInitialCertificate *certificate) {
    if (!program || !certificate || !left || !left_count || !right ||
        !right_count || (prefix_count && !prefix) || !limits.max_schedules ||
        !limits.max_commutation_states)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *certificate = (ConcurrencyNativeWeakInitialCertificate){
        .limits = limits,
        .prefix_count = prefix_count,
        .left_count = left_count,
        .right_count = right_count,
        .left_schedule_index = SIZE_MAX,
        .right_schedule_index = SIZE_MAX,
        .program_fingerprint = program_fingerprint(program),
        .prefix_fingerprint = prefix_fingerprint(prefix, prefix_count),
        .left_fingerprint = prefix_fingerprint(left, left_count),
        .right_fingerprint = prefix_fingerprint(right, right_count),
    };
    ConcurrencyNativeExploration exhaustive = {0};
    ConcurrencyNativeStatus status = concurrency_native_explore(
        program, limits, &exhaustive);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    size_t count = exhaustive.schedule_count;
    bool *right_candidate = calloc(count, sizeof(*right_candidate));
    bool *visited = calloc(count, sizeof(*visited));
    size_t *parent = malloc(count * sizeof(*parent));
    size_t *parent_swap = malloc(count * sizeof(*parent_swap));
    size_t *queue = malloc(count * sizeof(*queue));
    if (count && (!right_candidate || !visited || !parent ||
                  !parent_swap || !queue)) {
        status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        parent[i] = SIZE_MAX;
        right_candidate[i] = native_plan_matches_schedule(
            prefix, prefix_count, right, right_count,
            &exhaustive.schedules[i]);
    }
    size_t head = 0, tail = 0;
    for (size_t i = 0; i < count; i++)
        if (native_plan_matches_schedule(
                prefix, prefix_count, left, left_count,
                &exhaustive.schedules[i])) {
            visited[i] = true;
            queue[tail++] = i;
            if (certificate->left_schedule_index == SIZE_MAX)
                certificate->left_schedule_index = i;
        }
    size_t target = SIZE_MAX;
    while (head < tail && target == SIZE_MAX) {
        size_t current = queue[head++];
        if (right_candidate[current]) {
            target = current;
            break;
        }
        const ConcurrencyNativeSchedule *schedule =
            &exhaustive.schedules[current];
        for (size_t position = prefix_count;
             position + 1 < schedule->choice_count; position++) {
            ConcurrencyNativeCommutationCertificate diamond = {0};
            ConcurrencyNativeCommutationStatus commute =
                concurrency_native_commute(
                    program, schedule->choices[position].process,
                    schedule->choices[position + 1].process,
                    schedule->choices, position,
                    limits.max_commutation_states, &diamond);
            if (commute == CONCURRENCY_NATIVE_COMMUTATION_LIMIT_EXCEEDED) {
                status = CONCURRENCY_NATIVE_LIMIT_EXCEEDED;
                goto done;
            }
            if (commute == CONCURRENCY_NATIVE_COMMUTATION_OUT_OF_MEMORY) {
                status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
                goto done;
            }
            if (commute != CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED)
                continue;
            for (size_t next = 0; next < count; next++)
                if (!visited[next] && native_swap_neighbor(
                        schedule, &exhaustive.schedules[next], position)) {
                    visited[next] = true;
                    parent[next] = current;
                    parent_swap[next] = position;
                    queue[tail++] = next;
                    break;
                }
        }
    }
    if (target != SIZE_MAX) {
        certificate->related = true;
        certificate->right_schedule_index = target;
        certificate->left_schedule_index = target;
        while (parent[certificate->left_schedule_index] != SIZE_MAX)
            certificate->left_schedule_index =
                parent[certificate->left_schedule_index];
        size_t swaps = 0;
        for (size_t at = target; parent[at] != SIZE_MAX; at = parent[at])
            swaps++;
        if (swaps) {
            certificate->swap_positions = malloc(
                swaps * sizeof(*certificate->swap_positions));
            if (!certificate->swap_positions) {
                status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
                goto done;
            }
            certificate->swap_count = swaps;
            size_t at = target;
            for (size_t i = swaps; i > 0; i--) {
                certificate->swap_positions[i - 1] = parent_swap[at];
                at = parent[at];
            }
        }
    }
    certificate->fingerprint = native_weak_initial_fingerprint(certificate);
done:
    free(right_candidate);
    free(visited);
    free(parent);
    free(parent_swap);
    free(queue);
    concurrency_native_exploration_free(&exhaustive);
    if (status != CONCURRENCY_NATIVE_CERTIFIED)
        concurrency_native_weak_initial_free(certificate);
    return status;
}

ConcurrencyNativeStatus concurrency_native_weak_initial_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *left, size_t left_count,
    const ConcurrencyNativeChoice *right, size_t right_count,
    const ConcurrencyNativeWeakInitialCertificate *certificate) {
    if (!certificate || prefix_count != certificate->prefix_count ||
        left_count != certificate->left_count ||
        right_count != certificate->right_count ||
        (certificate->swap_count && !certificate->swap_positions))
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    if (certificate->swap_count > certificate->limits.max_schedules ||
        (certificate->related &&
         (certificate->left_schedule_index == SIZE_MAX ||
          certificate->right_schedule_index == SIZE_MAX)) ||
        (!certificate->related && certificate->swap_count))
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    if (certificate->fingerprint !=
        native_weak_initial_fingerprint(certificate))
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    ConcurrencyNativeWeakInitialCertificate expected = {0};
    ConcurrencyNativeStatus status = concurrency_native_weak_initial(
        program, prefix, prefix_count, left, left_count, right, right_count,
        certificate->limits, &expected);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    bool equal = certificate->fingerprint == expected.fingerprint;
    concurrency_native_weak_initial_free(&expected);
    return equal ? CONCURRENCY_NATIVE_CERTIFIED
                 : CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
}

static bool native_sequence_starts_with(
    const ConcurrencyNativeSchedule *schedule,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count) {
    if (prefix_count > schedule->choice_count) return false;
    for (size_t i = 0; i < prefix_count; i++)
        if (!native_choice_identity_equal(schedule->choices[i], prefix[i]))
            return false;
    return true;
}

static ConcurrencyNativeStatus native_validate_plan(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *sequence, size_t sequence_count) {
    if (prefix_count > SIZE_MAX - sequence_count ||
        prefix_count + sequence_count >
            SIZE_MAX / sizeof(ConcurrencyNativeChoice))
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    size_t count = prefix_count + sequence_count;
    ConcurrencyNativeChoice *plan = malloc(count * sizeof(*plan));
    if (!plan) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    if (prefix_count)
        memcpy(plan, prefix, prefix_count * sizeof(*plan));
    memcpy(plan + prefix_count, sequence,
           sequence_count * sizeof(*plan));
    ConcurrencyNativeFrontier frontier = {0};
    ConcurrencyNativeStatus status = concurrency_native_frontier(
        program, plan, count, &frontier);
    concurrency_native_frontier_free(&frontier);
    free(plan);
    return status;
}

static uint64_t native_initial_fingerprint(
    const ConcurrencyNativeInitialCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->limits.max_schedules);
    hash = mix(hash, certificate->limits.max_commutation_states);
    hash = mix(hash, certificate->limits.max_equivalence_states);
    hash = mix(hash, certificate->limits.max_wakeup_obligations);
    hash = mix(hash, certificate->prefix_count);
    hash = mix(hash, certificate->left_count);
    hash = mix(hash, certificate->right_count);
    hash = mix(hash, certificate->related);
    hash = mix(hash, certificate->reached_state_index);
    hash = mix(hash, certificate->swap_count);
    hash = mix(hash, certificate->program_fingerprint);
    hash = mix(hash, certificate->prefix_fingerprint);
    hash = mix(hash, certificate->left_fingerprint);
    hash = mix(hash, certificate->right_fingerprint);
    for (size_t i = 0; i < certificate->swap_count; i++)
        hash = mix(hash, certificate->swap_positions[i]);
    return hash;
}

void concurrency_native_initial_free(
    ConcurrencyNativeInitialCertificate *certificate) {
    if (!certificate) return;
    free(certificate->swap_positions);
    *certificate = (ConcurrencyNativeInitialCertificate){0};
}

ConcurrencyNativeStatus concurrency_native_initial(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *left, size_t left_count,
    const ConcurrencyNativeChoice *right, size_t right_count,
    ConcurrencyNativeLimits limits,
    ConcurrencyNativeInitialCertificate *certificate) {
    if (!program || !certificate || !left || !left_count || !right ||
        !right_count || left_count > right_count ||
        (prefix_count && !prefix) || !limits.max_commutation_states ||
        !limits.max_equivalence_states)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *certificate = (ConcurrencyNativeInitialCertificate){
        .limits = limits,
        .prefix_count = prefix_count,
        .left_count = left_count,
        .right_count = right_count,
        .reached_state_index = SIZE_MAX,
    };
    ConcurrencyNativeStatus status = native_validate_plan(
        program, prefix, prefix_count, right, right_count);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    status = native_validate_plan(
        program, prefix, prefix_count, left, left_count);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    if ((prefix_count && prefix[prefix_count - 1].tick == UINT64_MAX) ||
        right_count - 1 > UINT64_MAX -
            (prefix_count ? prefix[prefix_count - 1].tick + 1 : 0))
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    certificate->program_fingerprint = program_fingerprint(program);
    certificate->prefix_fingerprint = prefix_fingerprint(prefix, prefix_count);
    certificate->left_fingerprint = prefix_fingerprint(left, left_count);
    certificate->right_fingerprint = prefix_fingerprint(right, right_count);
    ConcurrencyNativeSchedule seed = {0};
    seed.choices = malloc(right_count * sizeof(*seed.choices));
    if (!seed.choices) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    memcpy(seed.choices, right, right_count * sizeof(*seed.choices));
    uint64_t tick = prefix_count ? prefix[prefix_count - 1].tick + 1 : 0;
    for (size_t i = 0; i < right_count; i++) seed.choices[i].tick = tick + i;
    seed.choice_count = right_count;
    seed.schedule_fingerprint = schedule_fingerprint(&seed);
    ConcurrencyNativeSchedule *states = NULL;
    size_t state_count = 0;
    status = native_schedule_array_append(&states, &state_count, &seed);
    free(seed.choices);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    size_t capacity = limits.max_equivalence_states;
    size_t *parent = malloc(capacity * sizeof(*parent));
    size_t *parent_swap = malloc(capacity * sizeof(*parent_swap));
    if (!parent || !parent_swap) {
        status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
        goto done;
    }
    parent[0] = SIZE_MAX;
    size_t target = SIZE_MAX;
    for (size_t current = 0; current < state_count; current++) {
        if (native_sequence_starts_with(&states[current], left, left_count)) {
            target = current;
            break;
        }
        for (size_t position = 0; position + 1 < right_count; position++) {
            size_t full_count = prefix_count + position;
            ConcurrencyNativeChoice *before = full_count
                ? malloc(full_count * sizeof(*before)) : NULL;
            if (full_count && !before) {
                status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
                goto done;
            }
            if (prefix_count)
                memcpy(before, prefix, prefix_count * sizeof(*before));
            if (position)
                memcpy(before + prefix_count, states[current].choices,
                       position * sizeof(*before));
            ConcurrencyNativeCommutationCertificate diamond = {0};
            ConcurrencyNativeCommutationStatus commute =
                concurrency_native_commute(
                    program, states[current].choices[position].process,
                    states[current].choices[position + 1].process,
                    before, full_count, limits.max_commutation_states,
                    &diamond);
            free(before);
            if (commute == CONCURRENCY_NATIVE_COMMUTATION_LIMIT_EXCEEDED) {
                status = CONCURRENCY_NATIVE_LIMIT_EXCEEDED;
                goto done;
            }
            if (commute == CONCURRENCY_NATIVE_COMMUTATION_OUT_OF_MEMORY) {
                status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
                goto done;
            }
            if (commute != CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED) continue;
            ConcurrencyNativeSchedule neighbor = {0};
            neighbor.choices = malloc(right_count * sizeof(*neighbor.choices));
            if (!neighbor.choices) {
                status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
                goto done;
            }
            memcpy(neighbor.choices, states[current].choices,
                   right_count * sizeof(*neighbor.choices));
            ConcurrencyNativeChoice swapped = neighbor.choices[position];
            neighbor.choices[position] = neighbor.choices[position + 1];
            neighbor.choices[position + 1] = swapped;
            for (size_t i = 0; i < right_count; i++)
                neighbor.choices[i].tick = tick + i;
            neighbor.choice_count = right_count;
            neighbor.schedule_fingerprint = schedule_fingerprint(&neighbor);
            bool known = false;
            for (size_t i = 0; i < state_count; i++)
                if (native_schedule_equal(&states[i], &neighbor)) {
                    known = true;
                    break;
                }
            if (known) {
                free(neighbor.choices);
                continue;
            }
            if (state_count == capacity) {
                free(neighbor.choices);
                status = CONCURRENCY_NATIVE_LIMIT_EXCEEDED;
                goto done;
            }
            status = native_schedule_array_append(
                &states, &state_count, &neighbor);
            free(neighbor.choices);
            if (status != CONCURRENCY_NATIVE_CERTIFIED) goto done;
            parent[state_count - 1] = current;
            parent_swap[state_count - 1] = position;
        }
    }
    if (target != SIZE_MAX) {
        certificate->related = true;
        certificate->reached_state_index = target;
        for (size_t at = target; parent[at] != SIZE_MAX; at = parent[at])
            certificate->swap_count++;
        if (certificate->swap_count) {
            certificate->swap_positions = malloc(
                certificate->swap_count * sizeof(*certificate->swap_positions));
            if (!certificate->swap_positions) {
                status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
                goto done;
            }
            size_t at = target;
            for (size_t i = certificate->swap_count; i > 0; i--) {
                certificate->swap_positions[i - 1] = parent_swap[at];
                at = parent[at];
            }
        }
    }
    certificate->fingerprint = native_initial_fingerprint(certificate);
done:
    free(parent);
    free(parent_swap);
    native_schedule_array_free(states, state_count);
    if (status != CONCURRENCY_NATIVE_CERTIFIED)
        concurrency_native_initial_free(certificate);
    return status;
}

ConcurrencyNativeStatus concurrency_native_initial_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *left, size_t left_count,
    const ConcurrencyNativeChoice *right, size_t right_count,
    const ConcurrencyNativeInitialCertificate *certificate) {
    if (!certificate || prefix_count != certificate->prefix_count ||
        left_count != certificate->left_count ||
        right_count != certificate->right_count ||
        (certificate->swap_count && !certificate->swap_positions))
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    if ((certificate->related &&
         certificate->reached_state_index == SIZE_MAX) ||
        (!certificate->related &&
         (certificate->reached_state_index != SIZE_MAX ||
          certificate->swap_count)) ||
        certificate->swap_count >
            certificate->limits.max_equivalence_states ||
        certificate->fingerprint != native_initial_fingerprint(certificate))
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    ConcurrencyNativeInitialCertificate expected = {0};
    ConcurrencyNativeStatus status = concurrency_native_initial(
        program, prefix, prefix_count, left, left_count, right, right_count,
        certificate->limits, &expected);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    bool equal = certificate->fingerprint == expected.fingerprint;
    concurrency_native_initial_free(&expected);
    return equal ? CONCURRENCY_NATIVE_CERTIFIED
                 : CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
}

static uint64_t native_wakeup_admissibility_fingerprint(
    const ConcurrencyNativeWakeupAdmissibilityCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->limits.max_schedules);
    hash = mix(hash, certificate->limits.max_commutation_states);
    hash = mix(hash, certificate->limits.max_equivalence_states);
    hash = mix(hash, certificate->limits.max_wakeup_obligations);
    hash = mix(hash, certificate->decision_count);
    hash = mix(hash, certificate->admissible);
    hash = mix(hash, certificate->blocking_sleep_index);
    hash = mix(hash, certificate->prefix_count);
    hash = mix(hash, certificate->sequence_count);
    hash = mix(hash, certificate->program_fingerprint);
    hash = mix(hash, certificate->prefix_fingerprint);
    hash = mix(hash, certificate->sequence_fingerprint);
    hash = mix(hash, certificate->sleep_fingerprint);
    for (size_t i = 0; i < certificate->decision_count; i++) {
        const ConcurrencyNativeWakeupAdmissibilityDecision *decision =
            &certificate->decisions[i];
        hash = mix(hash, decision->process.module_id);
        hash = mix(hash, decision->process.process_id);
        hash = mix(hash, decision->weak_initial.fingerprint);
    }
    return hash;
}

void concurrency_native_wakeup_admissibility_free(
    ConcurrencyNativeWakeupAdmissibilityCertificate *certificate) {
    if (!certificate) return;
    for (size_t i = 0; i < certificate->decision_count; i++)
        concurrency_native_weak_initial_free(
            &certificate->decisions[i].weak_initial);
    free(certificate->decisions);
    *certificate = (ConcurrencyNativeWakeupAdmissibilityCertificate){0};
}

ConcurrencyNativeStatus concurrency_native_wakeup_admissible(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *sequence, size_t sequence_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    ConcurrencyNativeLimits limits,
    ConcurrencyNativeWakeupAdmissibilityCertificate *certificate) {
    if (!program || !certificate || !sequence || !sequence_count ||
        (sleep_count && !sleep) || (prefix_count && !prefix) ||
        !limits.max_schedules || !limits.max_commutation_states)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *certificate = (ConcurrencyNativeWakeupAdmissibilityCertificate){0};
    for (size_t i = 0; i < sleep_count; i++) {
        if (!sleep[i].module_id || !sleep[i].process_id)
            return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
        for (size_t prior = 0; prior < i; prior++)
            if (process_equal(sleep[i], sleep[prior]))
                return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    }
    ConcurrencyNativeFrontier frontier = {0};
    ConcurrencyNativeStatus status = concurrency_native_frontier(
        program, prefix, prefix_count, &frontier);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    ConcurrencyNativeExploration exhaustive = {0};
    status = concurrency_native_explore(program, limits, &exhaustive);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) {
        concurrency_native_frontier_free(&frontier);
        return status;
    }
    bool sequence_enabled = false;
    for (size_t i = 0; i < exhaustive.schedule_count; i++)
        if (native_plan_matches_schedule(
                prefix, prefix_count, sequence, sequence_count,
                &exhaustive.schedules[i])) {
            sequence_enabled = true;
            break;
        }
    concurrency_native_exploration_free(&exhaustive);
    if (!sequence_enabled) {
        concurrency_native_frontier_free(&frontier);
        return CONCURRENCY_NATIVE_MALFORMED_PREFIX;
    }
    certificate->decisions = sleep_count
        ? calloc(sleep_count, sizeof(*certificate->decisions)) : NULL;
    if (sleep_count && !certificate->decisions) {
        concurrency_native_frontier_free(&frontier);
        return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
    }
    certificate->limits = limits;
    certificate->decision_count = sleep_count;
    certificate->admissible = true;
    certificate->blocking_sleep_index = SIZE_MAX;
    certificate->prefix_count = prefix_count;
    certificate->sequence_count = sequence_count;
    certificate->program_fingerprint = program_fingerprint(program);
    certificate->prefix_fingerprint = prefix_fingerprint(prefix, prefix_count);
    certificate->sequence_fingerprint =
        prefix_fingerprint(sequence, sequence_count);
    certificate->sleep_fingerprint =
        native_process_set_fingerprint(sleep, sleep_count);
    for (size_t i = 0; i < sleep_count; i++) {
        const ConcurrencyNativeFrontierEntry *entry = NULL;
        for (size_t j = 0; j < frontier.process_count; j++)
            if (process_equal(frontier.entries[j].process, sleep[i]))
                entry = &frontier.entries[j];
        if (!entry || !entry->enabled) {
            status = CONCURRENCY_NATIVE_MALFORMED_PREFIX;
            goto done;
        }
        ConcurrencyNativeChoice sleeping = {
            .process = sleep[i],
            .local_index = entry->local_index,
            .tick = prefix_count,
        };
        certificate->decisions[i].process = sleep[i];
        status = concurrency_native_weak_initial(
            program, prefix, prefix_count, &sleeping, 1,
            sequence, sequence_count, limits,
            &certificate->decisions[i].weak_initial);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) goto done;
        if (certificate->decisions[i].weak_initial.related) {
            certificate->admissible = false;
            if (certificate->blocking_sleep_index == SIZE_MAX)
                certificate->blocking_sleep_index = i;
        }
    }
    certificate->fingerprint =
        native_wakeup_admissibility_fingerprint(certificate);
done:
    concurrency_native_frontier_free(&frontier);
    if (status != CONCURRENCY_NATIVE_CERTIFIED)
        concurrency_native_wakeup_admissibility_free(certificate);
    return status;
}

ConcurrencyNativeStatus concurrency_native_wakeup_admissible_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *sequence, size_t sequence_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    const ConcurrencyNativeWakeupAdmissibilityCertificate *certificate) {
    if (!certificate ||
        (certificate->decision_count && !certificate->decisions) ||
        sleep_count != certificate->decision_count ||
        prefix_count != certificate->prefix_count ||
        sequence_count != certificate->sequence_count)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    size_t first_blocker = SIZE_MAX;
    for (size_t i = 0; i < certificate->decision_count; i++) {
        const ConcurrencyNativeWakeupAdmissibilityDecision *decision =
            &certificate->decisions[i];
        if (!process_equal(decision->process, sleep[i]) ||
            decision->weak_initial.fingerprint !=
                native_weak_initial_fingerprint(&decision->weak_initial))
            return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
        if (first_blocker == SIZE_MAX && decision->weak_initial.related)
            first_blocker = i;
    }
    if (certificate->admissible != (first_blocker == SIZE_MAX) ||
        certificate->blocking_sleep_index != first_blocker ||
        certificate->fingerprint !=
            native_wakeup_admissibility_fingerprint(certificate))
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    ConcurrencyNativeWakeupAdmissibilityCertificate expected = {0};
    ConcurrencyNativeStatus status = concurrency_native_wakeup_admissible(
        program, prefix, prefix_count, sequence, sequence_count,
        sleep, sleep_count, certificate->limits, &expected);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    bool equal = certificate->fingerprint == expected.fingerprint;
    concurrency_native_wakeup_admissibility_free(&expected);
    return equal ? CONCURRENCY_NATIVE_CERTIFIED
                 : CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
}

static uint64_t native_wakeup_nodes_fingerprint(
    const ConcurrencyNativeWakeupNode *nodes, size_t count) {
    uint64_t hash = mix(UINT64_C(1469598103934665603), count);
    for (size_t i = 0; i < count; i++) {
        hash = mix(hash, nodes[i].parent_index);
        hash = mix(hash, nodes[i].choice.process.module_id);
        hash = mix(hash, nodes[i].choice.process.process_id);
        hash = mix(hash, nodes[i].choice.local_index);
        hash = mix(hash, nodes[i].depth);
        hash = mix(hash, nodes[i].terminal);
    }
    return hash;
}

static uint64_t native_wakeup_insert_input_fingerprint(
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    const ConcurrencyNativeSchedule *leaves, size_t leaf_count,
    const ConcurrencyNativeChoice *candidate, size_t candidate_count) {
    uint64_t hash = prefix_fingerprint(prefix, prefix_count);
    hash = mix(hash, native_process_set_fingerprint(sleep, sleep_count));
    hash = mix(hash, leaf_count);
    for (size_t i = 0; i < leaf_count; i++)
        hash = mix(hash, prefix_fingerprint(
            leaves[i].choices, leaves[i].choice_count));
    return mix(hash, prefix_fingerprint(candidate, candidate_count));
}

static uint64_t native_wakeup_insert_fingerprint(
    const ConcurrencyNativeWakeupInsertCertificate *certificate) {
    uint64_t hash = native_wakeup_nodes_fingerprint(
        certificate->nodes, certificate->node_count);
    hash = mix(hash, certificate->limits.max_schedules);
    hash = mix(hash, certificate->limits.max_commutation_states);
    hash = mix(hash, certificate->limits.max_equivalence_states);
    hash = mix(hash, certificate->before_node_count);
    hash = mix(hash, certificate->suppressed);
    hash = mix(hash, certificate->anchor_node_index);
    hash = mix(hash, certificate->added_leaf_node_index);
    hash = mix(hash, certificate->prefix_count);
    hash = mix(hash, certificate->candidate_count);
    hash = mix(hash, certificate->sleep_count);
    hash = mix(hash, certificate->existing_leaf_count);
    hash = mix(hash, certificate->program_fingerprint);
    return mix(hash, certificate->input_fingerprint);
}

void concurrency_native_wakeup_insert_free(
    ConcurrencyNativeWakeupInsertCertificate *certificate) {
    if (!certificate) return;
    free(certificate->nodes);
    *certificate = (ConcurrencyNativeWakeupInsertCertificate){0};
}

static void native_wakeup_postorder(
    const ConcurrencyNativeWakeupNode *nodes, size_t count, size_t parent,
    size_t *order, size_t *cursor) {
    for (size_t i = 1; i < count; i++)
        if (nodes[i].parent_index == parent)
            native_wakeup_postorder(nodes, count, i, order, cursor);
    order[(*cursor)++] = parent;
}

static ConcurrencyNativeChoice *native_wakeup_node_sequence(
    const ConcurrencyNativeWakeupNode *nodes, size_t node) {
    size_t count = nodes[node].depth;
    ConcurrencyNativeChoice *sequence = count
        ? malloc(count * sizeof(*sequence)) : NULL;
    for (size_t at = node, i = count; i > 0; at = nodes[at].parent_index)
        sequence[--i] = nodes[at].choice;
    return sequence;
}

static ConcurrencyNativeStatus native_validate_ordered_wakeup_tree(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    const ConcurrencyNativeWakeupNode *nodes, size_t node_count,
    ConcurrencyNativeLimits limits) {
    if (!node_count || nodes[0].parent_index != SIZE_MAX ||
        nodes[0].depth || nodes[0].terminal)
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    for (size_t node = 1; node < node_count; node++) {
        if (nodes[node].parent_index >= node ||
            nodes[node].depth != nodes[nodes[node].parent_index].depth + 1)
            return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
        bool child = false;
        for (size_t i = node + 1; i < node_count; i++)
            if (nodes[i].parent_index == node) child = true;
        if (nodes[node].terminal && child)
            return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
        if (!nodes[node].terminal) continue;
        ConcurrencyNativeChoice *leaf = native_wakeup_node_sequence(nodes, node);
        if (!leaf) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
        ConcurrencyNativeWakeupAdmissibilityCertificate admissibility = {0};
        ConcurrencyNativeStatus status = concurrency_native_wakeup_admissible(
            program, prefix, prefix_count, leaf, nodes[node].depth,
            sleep, sleep_count, limits, &admissibility);
        free(leaf);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
        bool admissible = admissibility.admissible;
        concurrency_native_wakeup_admissibility_free(&admissibility);
        if (!admissible) return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
        leaf = native_wakeup_node_sequence(nodes, node);
        if (!leaf) return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
        size_t path_node = 0;
        for (size_t depth = 0; depth < nodes[node].depth; depth++) {
            size_t chosen = SIZE_MAX;
            for (size_t i = 1; i < node_count; i++)
                if (nodes[i].parent_index == path_node &&
                    native_choice_identity_equal(nodes[i].choice, leaf[depth])) {
                    chosen = i;
                    break;
                }
            if (chosen == SIZE_MAX) {
                free(leaf);
                return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
            }
            for (size_t sibling = 1; sibling < chosen; sibling++) {
                if (nodes[sibling].parent_index != path_node) continue;
                size_t combined_count = prefix_count + depth;
                ConcurrencyNativeChoice *combined = combined_count
                    ? malloc(combined_count * sizeof(*combined)) : NULL;
                if (combined_count && !combined) {
                    free(leaf);
                    return CONCURRENCY_NATIVE_OUT_OF_MEMORY;
                }
                if (prefix_count)
                    memcpy(combined, prefix,
                           prefix_count * sizeof(*combined));
                if (depth)
                    memcpy(combined + prefix_count, leaf,
                           depth * sizeof(*combined));
                ConcurrencyNativeWeakInitialCertificate weak = {0};
                status = concurrency_native_weak_initial(
                    program, combined, combined_count,
                    &nodes[sibling].choice, 1, leaf + depth,
                    nodes[node].depth - depth, limits, &weak);
                free(combined);
                if (status != CONCURRENCY_NATIVE_CERTIFIED) {
                    free(leaf);
                    return status;
                }
                bool related = weak.related;
                concurrency_native_weak_initial_free(&weak);
                if (related) {
                    free(leaf);
                    return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
                }
            }
            path_node = chosen;
        }
        free(leaf);
    }
    return CONCURRENCY_NATIVE_CERTIFIED;
}

ConcurrencyNativeStatus concurrency_native_wakeup_insert(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    const ConcurrencyNativeSchedule *existing_leaves, size_t existing_leaf_count,
    const ConcurrencyNativeChoice *candidate, size_t candidate_count,
    ConcurrencyNativeLimits limits,
    ConcurrencyNativeWakeupInsertCertificate *certificate) {
    if (!program || !certificate || !candidate || !candidate_count ||
        (prefix_count && !prefix) || (sleep_count && !sleep) ||
        (existing_leaf_count && !existing_leaves) ||
        !limits.max_schedules || !limits.max_commutation_states ||
        !limits.max_equivalence_states)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    *certificate = (ConcurrencyNativeWakeupInsertCertificate){0};
    ConcurrencyNativeWakeupAdmissibilityCertificate admissibility = {0};
    ConcurrencyNativeStatus status = concurrency_native_wakeup_admissible(
        program, prefix, prefix_count, candidate, candidate_count,
        sleep, sleep_count, limits, &admissibility);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    if (!admissibility.admissible) {
        concurrency_native_wakeup_admissibility_free(&admissibility);
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    }
    concurrency_native_wakeup_admissibility_free(&admissibility);
    ConcurrencyNativeExploration tree = {0};
    size_t root = 0;
    status = native_append_wakeup_node(
        &tree, SIZE_MAX, (ConcurrencyNativeChoice){0}, 0, &root);
    for (size_t i = 0;
         status == CONCURRENCY_NATIVE_CERTIFIED && i < existing_leaf_count;
         i++) {
        status = native_validate_plan(
            program, prefix, prefix_count,
            existing_leaves[i].choices, existing_leaves[i].choice_count);
        if (status == CONCURRENCY_NATIVE_CERTIFIED)
            status = native_insert_wakeup_schedule(&tree, &existing_leaves[i]);
    }
    if (status != CONCURRENCY_NATIVE_CERTIFIED) goto done;
    status = native_validate_ordered_wakeup_tree(
        program, prefix, prefix_count, sleep, sleep_count,
        tree.wakeup_nodes, tree.wakeup_node_count, limits);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) goto done;
    size_t before = tree.wakeup_node_count;
    size_t *order = malloc(before * sizeof(*order));
    if (!order) {
        status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
        goto done;
    }
    size_t cursor = 0;
    native_wakeup_postorder(tree.wakeup_nodes, before, 0, order, &cursor);
    size_t anchor = SIZE_MAX;
    for (size_t oi = 0; oi < cursor; oi++) {
        size_t node = order[oi];
        if (!node) {
            anchor = 0;
            break;
        }
        ConcurrencyNativeChoice *sequence = native_wakeup_node_sequence(
            tree.wakeup_nodes, node);
        if (tree.wakeup_nodes[node].depth && !sequence) {
            status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
            free(order);
            goto done;
        }
        ConcurrencyNativeWeakInitialCertificate weak = {0};
        status = concurrency_native_weak_initial(
            program, prefix, prefix_count, sequence,
            tree.wakeup_nodes[node].depth, candidate, candidate_count,
            limits, &weak);
        free(sequence);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) {
            free(order);
            goto done;
        }
        bool related = weak.related;
        concurrency_native_weak_initial_free(&weak);
        if (related) {
            anchor = node;
            break;
        }
    }
    free(order);
    if (anchor == SIZE_MAX) {
        status = CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
        goto done;
    }
    certificate->suppressed = tree.wakeup_nodes[anchor].terminal;
    certificate->anchor_node_index = anchor;
    certificate->added_leaf_node_index = SIZE_MAX;
    if (!certificate->suppressed) {
        ConcurrencyNativeChoice *anchor_sequence = native_wakeup_node_sequence(
            tree.wakeup_nodes, anchor);
        if (tree.wakeup_nodes[anchor].depth && !anchor_sequence) {
            status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
            goto done;
        }
        ConcurrencyNativeExploration exhaustive = {0};
        status = concurrency_native_explore(program, limits, &exhaustive);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) {
            free(anchor_sequence);
            goto done;
        }
        ConcurrencyNativeChoice *extension = NULL;
        size_t extension_count = 0;
        size_t shortest = candidate_count;
        if (shortest <= tree.wakeup_nodes[anchor].depth)
            shortest = tree.wakeup_nodes[anchor].depth + 1;
        for (size_t length = shortest;
             !extension && length <= exhaustive.schedules[0].choice_count -
                 prefix_count;
             length++)
            for (size_t i = 0; i < exhaustive.schedule_count; i++) {
                const ConcurrencyNativeSchedule *schedule =
                    &exhaustive.schedules[i];
                if (prefix_count + length > schedule->choice_count ||
                    !native_plan_matches_schedule(
                        prefix, prefix_count, anchor_sequence,
                        tree.wakeup_nodes[anchor].depth, schedule))
                    continue;
                ConcurrencyNativeInitialCertificate initial = {0};
                status = concurrency_native_initial(
                    program, prefix, prefix_count, candidate, candidate_count,
                    schedule->choices + prefix_count, length, limits, &initial);
                if (status != CONCURRENCY_NATIVE_CERTIFIED) break;
                bool related = initial.related;
                concurrency_native_initial_free(&initial);
                if (!related) continue;
                extension = malloc(length * sizeof(*extension));
                if (!extension) {
                    status = CONCURRENCY_NATIVE_OUT_OF_MEMORY;
                    break;
                }
                memcpy(extension, schedule->choices + prefix_count,
                       length * sizeof(*extension));
                extension_count = length;
                break;
            }
        free(anchor_sequence);
        concurrency_native_exploration_free(&exhaustive);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) {
            free(extension);
            goto done;
        }
        if (!extension) {
            status = CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
            goto done;
        }
        ConcurrencyNativeSchedule added = {
            .choices = extension,
            .choice_count = extension_count,
        };
        status = native_insert_wakeup_schedule(&tree, &added);
        free(extension);
        if (status != CONCURRENCY_NATIVE_CERTIFIED) goto done;
        for (size_t i = before; i < tree.wakeup_node_count; i++)
            if (tree.wakeup_nodes[i].terminal)
                certificate->added_leaf_node_index = i;
        if (certificate->added_leaf_node_index == SIZE_MAX) {
            status = CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
            goto done;
        }
    }
    status = native_validate_ordered_wakeup_tree(
        program, prefix, prefix_count, sleep, sleep_count,
        tree.wakeup_nodes, tree.wakeup_node_count, limits);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) goto done;
    certificate->limits = limits;
    certificate->nodes = tree.wakeup_nodes;
    certificate->node_count = tree.wakeup_node_count;
    certificate->before_node_count = before;
    certificate->prefix_count = prefix_count;
    certificate->candidate_count = candidate_count;
    certificate->sleep_count = sleep_count;
    certificate->existing_leaf_count = existing_leaf_count;
    certificate->program_fingerprint = program_fingerprint(program);
    certificate->input_fingerprint = native_wakeup_insert_input_fingerprint(
        prefix, prefix_count, sleep, sleep_count, existing_leaves,
        existing_leaf_count, candidate, candidate_count);
    certificate->fingerprint = native_wakeup_insert_fingerprint(certificate);
    tree.wakeup_nodes = NULL;
done:
    free(tree.wakeup_nodes);
    if (status != CONCURRENCY_NATIVE_CERTIFIED)
        concurrency_native_wakeup_insert_free(certificate);
    return status;
}

ConcurrencyNativeStatus concurrency_native_wakeup_insert_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    const ConcurrencyNativeSchedule *existing_leaves, size_t existing_leaf_count,
    const ConcurrencyNativeChoice *candidate, size_t candidate_count,
    const ConcurrencyNativeWakeupInsertCertificate *certificate) {
    if (!certificate || !certificate->nodes ||
        prefix_count != certificate->prefix_count ||
        candidate_count != certificate->candidate_count ||
        sleep_count != certificate->sleep_count ||
        existing_leaf_count != certificate->existing_leaf_count)
        return CONCURRENCY_NATIVE_INVALID_ARGUMENT;
    if (!certificate->node_count ||
        certificate->before_node_count > certificate->node_count ||
        certificate->anchor_node_index >= certificate->before_node_count ||
        certificate->fingerprint != native_wakeup_insert_fingerprint(certificate))
        return CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
    ConcurrencyNativeWakeupInsertCertificate expected = {0};
    ConcurrencyNativeStatus status = concurrency_native_wakeup_insert(
        program, prefix, prefix_count, sleep, sleep_count,
        existing_leaves, existing_leaf_count, candidate, candidate_count,
        certificate->limits, &expected);
    if (status != CONCURRENCY_NATIVE_CERTIFIED) return status;
    bool equal = certificate->fingerprint == expected.fingerprint;
    concurrency_native_wakeup_insert_free(&expected);
    return equal ? CONCURRENCY_NATIVE_CERTIFIED
                 : CONCURRENCY_NATIVE_INVALID_CERTIFICATE;
}

static size_t native_representative_for(
    const ConcurrencyNativeExploration *reduced,
    const ConcurrencyNativeSchedule *schedule) {
    for (size_t i = 0; i < reduced->schedule_count; i++)
        if (native_schedule_equal(schedule, &reduced->schedules[i])) return i;
    return reduced->schedule_count;
}

static uint64_t native_coverage_fingerprint(
    const ConcurrencyNativeCoverage *coverage) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, coverage->limits.max_swaps_per_witness);
    hash = mix(hash, coverage->limits.max_commutation_states);
    hash = mix(hash, coverage->exhaustive_exploration_fingerprint);
    hash = mix(hash, coverage->reduced_exploration_fingerprint);
    hash = mix(hash, coverage->witness_count);
    hash = mix(hash, coverage->covered_schedule_count);
    hash = mix(hash, coverage->total_swap_count);
    for (size_t i = 0; i < coverage->witness_count; i++) {
        const ConcurrencyNativeCoverageWitness *witness =
            &coverage->witnesses[i];
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

void concurrency_native_coverage_free(ConcurrencyNativeCoverage *coverage) {
    if (!coverage) return;
    for (size_t i = 0; i < coverage->witness_count; i++)
        free(coverage->witnesses[i].swap_positions);
    free(coverage->witnesses);
    *coverage = (ConcurrencyNativeCoverage){0};
}

static ConcurrencyNativeCoverageStatus native_build_coverage_witness(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeExploration *exhaustive,
    const ConcurrencyNativeExploration *reduced,
    size_t source_index, ConcurrencyNativeCoverageLimits limits,
    ConcurrencyNativeCoverageWitness *witness) {
    size_t count = exhaustive->schedule_count;
    if (!count || count > SIZE_MAX / sizeof(size_t))
        return CONCURRENCY_NATIVE_COVERAGE_INVALID_EXPLORATION;
    bool *visited = calloc(count, sizeof(*visited));
    size_t *parent = malloc(count * sizeof(*parent));
    size_t *parent_swap = malloc(count * sizeof(*parent_swap));
    size_t *queue = malloc(count * sizeof(*queue));
    if (!visited || !parent || !parent_swap || !queue) {
        free(visited); free(parent); free(parent_swap); free(queue);
        return CONCURRENCY_NATIVE_COVERAGE_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < count; i++) parent[i] = SIZE_MAX;
    size_t head = 0;
    size_t tail = 0;
    queue[tail++] = source_index;
    visited[source_index] = true;
    size_t target = count;
    size_t representative = reduced->schedule_count;
    ConcurrencyNativeCoverageStatus result =
        CONCURRENCY_NATIVE_COVERAGE_CERTIFIED;
    while (head < tail && target == count) {
        size_t current = queue[head++];
        const ConcurrencyNativeSchedule *schedule =
            &exhaustive->schedules[current];
        representative = native_representative_for(reduced, schedule);
        if (representative < reduced->schedule_count) {
            target = current;
            break;
        }
        for (size_t position = 0;
             position + 1 < schedule->choice_count; position++) {
            ConcurrencyNativeCommutationCertificate certificate = {0};
            ConcurrencyNativeCommutationStatus commute =
                concurrency_native_commute(
                    program,
                    schedule->choices[position].process,
                    schedule->choices[position + 1].process,
                    schedule->choices, position,
                    limits.max_commutation_states, &certificate);
            if (commute == CONCURRENCY_NATIVE_COMMUTATION_LIMIT_EXCEEDED) {
                result = CONCURRENCY_NATIVE_COVERAGE_LIMIT_EXCEEDED;
                goto done;
            }
            if (commute == CONCURRENCY_NATIVE_COMMUTATION_OUT_OF_MEMORY) {
                result = CONCURRENCY_NATIVE_COVERAGE_OUT_OF_MEMORY;
                goto done;
            }
            if (commute != CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED) continue;
            for (size_t next = 0; next < count; next++) {
                if (visited[next] || !native_swap_neighbor(
                        schedule, &exhaustive->schedules[next], position))
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
        result = CONCURRENCY_NATIVE_COVERAGE_UNCOVERED_SCHEDULE;
        goto done;
    }
    size_t swap_count = 0;
    for (size_t cursor = target; cursor != source_index;
         cursor = parent[cursor]) {
        if (cursor == SIZE_MAX || parent[cursor] == SIZE_MAX) {
            result = CONCURRENCY_NATIVE_COVERAGE_INVALID_EXPLORATION;
            goto done;
        }
        swap_count++;
    }
    if (swap_count > limits.max_swaps_per_witness) {
        result = CONCURRENCY_NATIVE_COVERAGE_LIMIT_EXCEEDED;
        goto done;
    }
    size_t *swaps = swap_count ? malloc(swap_count * sizeof(*swaps)) : NULL;
    if (swap_count && !swaps) {
        result = CONCURRENCY_NATIVE_COVERAGE_OUT_OF_MEMORY;
        goto done;
    }
    size_t cursor = target;
    for (size_t i = swap_count; i > 0; i--) {
        swaps[i - 1] = parent_swap[cursor];
        cursor = parent[cursor];
    }
    *witness = (ConcurrencyNativeCoverageWitness){
        .exhaustive_schedule_index = source_index,
        .representative_schedule_index = representative,
        .exhaustive_schedule_fingerprint =
            exhaustive->schedules[source_index].schedule_fingerprint,
        .representative_schedule_fingerprint =
            reduced->schedules[representative].schedule_fingerprint,
        .swap_positions = swaps,
        .swap_count = swap_count,
    };
done:
    free(visited); free(parent); free(parent_swap); free(queue);
    return result;
}

ConcurrencyNativeCoverageStatus concurrency_native_coverage_build(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeExploration *exhaustive,
    const ConcurrencyNativeExploration *reduced,
    ConcurrencyNativeCoverageLimits limits,
    ConcurrencyNativeCoverage *coverage) {
    if (!program || !exhaustive || !reduced || !coverage ||
        !limits.max_swaps_per_witness || !limits.max_commutation_states ||
        exhaustive->mode != CONCURRENCY_NATIVE_EXPLORATION_EXHAUSTIVE ||
        (reduced->mode !=
             CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_REDUCED &&
         reduced->mode !=
             CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_WAKEUP_TREE &&
         reduced->mode !=
             CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ONLINE_WAKEUP_TREE &&
         reduced->mode !=
             CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_RACE_WAKEUP &&
         reduced->mode !=
             CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ORDERED_WAKEUP) ||
        !exhaustive->schedule_count || !reduced->schedule_count)
        return CONCURRENCY_NATIVE_COVERAGE_INVALID_ARGUMENT;
    *coverage = (ConcurrencyNativeCoverage){
        .limits = limits,
        .exhaustive_exploration_fingerprint = exhaustive->fingerprint,
        .reduced_exploration_fingerprint = reduced->fingerprint,
    };
    if (concurrency_native_exploration_verify(program, exhaustive) !=
            CONCURRENCY_NATIVE_CERTIFIED ||
        concurrency_native_exploration_verify(program, reduced) !=
            CONCURRENCY_NATIVE_CERTIFIED)
        return CONCURRENCY_NATIVE_COVERAGE_INVALID_EXPLORATION;
    if (exhaustive->schedule_count >
        SIZE_MAX / sizeof(ConcurrencyNativeCoverageWitness))
        return CONCURRENCY_NATIVE_COVERAGE_OUT_OF_MEMORY;
    coverage->witnesses = calloc(
        exhaustive->schedule_count, sizeof(*coverage->witnesses));
    if (!coverage->witnesses)
        return CONCURRENCY_NATIVE_COVERAGE_OUT_OF_MEMORY;
    coverage->witness_count = exhaustive->schedule_count;
    for (size_t i = 0; i < exhaustive->schedule_count; i++) {
        ConcurrencyNativeCoverageStatus status = native_build_coverage_witness(
            program, exhaustive, reduced, i, limits, &coverage->witnesses[i]);
        if (status != CONCURRENCY_NATIVE_COVERAGE_CERTIFIED) {
            concurrency_native_coverage_free(coverage);
            return status;
        }
        coverage->covered_schedule_count++;
        if (coverage->total_swap_count >
            SIZE_MAX - coverage->witnesses[i].swap_count) {
            concurrency_native_coverage_free(coverage);
            return CONCURRENCY_NATIVE_COVERAGE_OUT_OF_MEMORY;
        }
        coverage->total_swap_count += coverage->witnesses[i].swap_count;
    }
    coverage->fingerprint = native_coverage_fingerprint(coverage);
    return CONCURRENCY_NATIVE_COVERAGE_CERTIFIED;
}

static ConcurrencyNativeCoverageStatus native_verify_coverage_witness(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeExploration *exhaustive,
    const ConcurrencyNativeExploration *reduced,
    const ConcurrencyNativeCoverageWitness *witness, size_t source_index,
    ConcurrencyNativeCoverageLimits limits) {
    if (witness->exhaustive_schedule_index != source_index ||
        witness->representative_schedule_index >= reduced->schedule_count ||
        witness->swap_count > limits.max_swaps_per_witness ||
        (witness->swap_count && !witness->swap_positions))
        return CONCURRENCY_NATIVE_COVERAGE_INVALID_CERTIFICATE;
    const ConcurrencyNativeSchedule *source =
        &exhaustive->schedules[source_index];
    const ConcurrencyNativeSchedule *representative =
        &reduced->schedules[witness->representative_schedule_index];
    if (source->choice_count != representative->choice_count ||
        witness->exhaustive_schedule_fingerprint !=
            source->schedule_fingerprint ||
        witness->representative_schedule_fingerprint !=
            representative->schedule_fingerprint ||
        source->choice_count > SIZE_MAX / sizeof(ConcurrencyNativeChoice))
        return CONCURRENCY_NATIVE_COVERAGE_INVALID_CERTIFICATE;
    ConcurrencyNativeChoice *choices = malloc(
        source->choice_count * sizeof(*choices));
    if (!choices) return CONCURRENCY_NATIVE_COVERAGE_OUT_OF_MEMORY;
    for (size_t i = 0; i < source->choice_count; i++)
        choices[i] = source->choices[i];
    for (size_t i = 0; i < witness->swap_count; i++) {
        size_t position = witness->swap_positions[i];
        if (position + 1 >= source->choice_count) {
            free(choices);
            return CONCURRENCY_NATIVE_COVERAGE_INVALID_CERTIFICATE;
        }
        ConcurrencyNativeCommutationCertificate certificate = {0};
        if (concurrency_native_commute(
                program, choices[position].process,
                choices[position + 1].process, choices, position,
                limits.max_commutation_states, &certificate) !=
            CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED) {
            free(choices);
            return CONCURRENCY_NATIVE_COVERAGE_INVALID_CERTIFICATE;
        }
        uint64_t left_tick = choices[position].tick;
        uint64_t right_tick = choices[position + 1].tick;
        ConcurrencyNativeChoice temporary = choices[position];
        choices[position] = choices[position + 1];
        choices[position + 1] = temporary;
        choices[position].tick = left_tick;
        choices[position + 1].tick = right_tick;
    }
    bool equal = true;
    for (size_t i = 0; i < source->choice_count; i++)
        if (!native_choice_identity_equal(
                choices[i], representative->choices[i])) {
            equal = false;
            break;
        }
    free(choices);
    return equal ? CONCURRENCY_NATIVE_COVERAGE_CERTIFIED
                 : CONCURRENCY_NATIVE_COVERAGE_INVALID_CERTIFICATE;
}

ConcurrencyNativeCoverageStatus concurrency_native_coverage_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeExploration *exhaustive,
    const ConcurrencyNativeExploration *reduced,
    const ConcurrencyNativeCoverage *coverage) {
    if (!program || !exhaustive || !reduced || !coverage ||
        !coverage->limits.max_swaps_per_witness ||
        !coverage->limits.max_commutation_states)
        return CONCURRENCY_NATIVE_COVERAGE_INVALID_ARGUMENT;
    if ((coverage->witness_count && !coverage->witnesses) ||
        coverage->exhaustive_exploration_fingerprint !=
            exhaustive->fingerprint ||
        coverage->reduced_exploration_fingerprint != reduced->fingerprint ||
        coverage->witness_count != exhaustive->schedule_count ||
        coverage->covered_schedule_count != exhaustive->schedule_count ||
        coverage->fingerprint != native_coverage_fingerprint(coverage))
        return CONCURRENCY_NATIVE_COVERAGE_INVALID_CERTIFICATE;
    if (concurrency_native_exploration_verify(program, exhaustive) !=
            CONCURRENCY_NATIVE_CERTIFIED ||
        concurrency_native_exploration_verify(program, reduced) !=
            CONCURRENCY_NATIVE_CERTIFIED)
        return CONCURRENCY_NATIVE_COVERAGE_INVALID_EXPLORATION;
    size_t total_swaps = 0;
    for (size_t i = 0; i < coverage->witness_count; i++) {
        ConcurrencyNativeCoverageStatus status =
            native_verify_coverage_witness(
                program, exhaustive, reduced, &coverage->witnesses[i], i,
                coverage->limits);
        if (status != CONCURRENCY_NATIVE_COVERAGE_CERTIFIED) return status;
        if (total_swaps > SIZE_MAX - coverage->witnesses[i].swap_count)
            return CONCURRENCY_NATIVE_COVERAGE_INVALID_CERTIFICATE;
        total_swaps += coverage->witnesses[i].swap_count;
    }
    return total_swaps == coverage->total_swap_count
        ? CONCURRENCY_NATIVE_COVERAGE_CERTIFIED
        : CONCURRENCY_NATIVE_COVERAGE_INVALID_CERTIFICATE;
}
