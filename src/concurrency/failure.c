#include "failure.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
    FAILURE_WIRE_VERSION = 1,
    FAILURE_WIRE_HEADER_SIZE = 72,
    FAILURE_WIRE_NODE_SIZE = 56,
};

static const uint8_t failure_wire_magic[4] = {'M', 'C', 'F', 'C'};

static void put_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static uint16_t get_u16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void put_u64(uint8_t *bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; i++)
        bytes[i] = (uint8_t)(value >> (56 - 8 * i));
}

static uint64_t get_u64(const uint8_t *bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++)
        value = (value << 8) | bytes[i];
    return value;
}

typedef struct {
    ConcurrencyTaskId task;
    ConcurrencyTaskId parent;
    bool cancellation_pending;
} FailureTask;

static bool task_equal(ConcurrencyTaskId left, ConcurrencyTaskId right) {
    return left.module_id == right.module_id && left.task_id == right.task_id;
}

static size_t find_task(
    const FailureTask *tasks, size_t count, ConcurrencyTaskId task) {
    for (size_t i = 0; i < count; i++)
        if (task_equal(tasks[i].task, task)) return i;
    return count;
}

static size_t find_primary(
    const ConcurrencyFailureNode *nodes, size_t count,
    ConcurrencyTaskId task) {
    for (size_t i = 0; i < count; i++)
        if (task_equal(nodes[i].task, task) &&
            nodes[i].cause != CONCURRENCY_FAILURE_CLEANUP)
            return i;
    return count;
}

static int compare_nodes(const void *left_ptr, const void *right_ptr) {
    const ConcurrencyFailureNode *left = left_ptr;
    const ConcurrencyFailureNode *right = right_ptr;
    if (left->task.module_id != right->task.module_id)
        return left->task.module_id < right->task.module_id ? -1 : 1;
    if (left->task.task_id != right->task.task_id)
        return left->task.task_id < right->task.task_id ? -1 : 1;
    if (left->cause != right->cause)
        return left->cause < right->cause ? -1 : 1;
    if (left->step_index == right->step_index) return 0;
    return left->step_index < right->step_index ? -1 : 1;
}

static uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t certificate_fingerprint(
    const ConcurrencyFailureCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->node_count);
    hash = mix(hash, certificate->root_count);
    hash = mix(hash, certificate->total_grade.finite);
    hash = mix(hash, certificate->total_grade.is_omega ? 1 : 0);
    hash = mix(hash, certificate->effect_fingerprint);
    hash = mix(hash, certificate->scope_capability_id);
    hash = mix(hash, certificate->task_type_id);
    for (size_t i = 0; i < certificate->node_count; i++) {
        const ConcurrencyFailureNode *node = &certificate->nodes[i];
        hash = mix(hash, node->task.module_id);
        hash = mix(hash, node->task.task_id);
        hash = mix(hash, (uint64_t)node->cause);
        hash = mix(hash, node->parent_index);
        hash = mix(hash, node->step_index);
        hash = mix(hash, node->grade.finite);
        hash = mix(hash, node->grade.is_omega ? 1 : 0);
    }
    return hash;
}

static ConcurrencyFailureCertificateStatus populate(
    const ConcurrencyTrace *trace,
    const ConcurrencyFailureAuthority *authority,
    ConcurrencyFailureCertificate *certificate) {
    if (!authority || !authority->solver || !authority->effects ||
        !authority->scope_capability_id || !authority->task_type_id)
        return CONCURRENCY_FAILURE_INVALID_ARGUMENT;
    if (concurrency_verify_trace_effects(
            trace, authority->solver, authority->effects,
            authority->scope_capability_id, authority->task_type_id) !=
        CONCURRENCY_EFFECTS_VALID)
        return CONCURRENCY_FAILURE_INVALID_TRACE;
    ConcurrencyVerification verified = concurrency_verify_trace(trace);
    if (trace->step_count == SIZE_MAX)
        return CONCURRENCY_FAILURE_OUT_OF_MEMORY;

    size_t node_count = verified.primary_failure_count +
        verified.cleanup_failure_count;
    size_t task_capacity = trace->step_count + 1;
    FailureTask *tasks = calloc(task_capacity, sizeof(*tasks));
    ConcurrencyFailureNode *nodes = node_count
        ? calloc(node_count, sizeof(*nodes)) : NULL;
    if (!tasks || (node_count && !nodes)) {
        free(tasks);
        free(nodes);
        return CONCURRENCY_FAILURE_OUT_OF_MEMORY;
    }

    size_t task_count = 1;
    size_t emitted = 0;
    tasks[0].task = trace->root;
    for (size_t i = 0; i < trace->step_count; i++) {
        const ConcurrencyStep *step = &trace->steps[i];
        if (step->kind == CONCURRENCY_STEP_SPAWN) {
            tasks[task_count++] = (FailureTask){
                .task = step->child,
                .parent = step->parent,
            };
        } else if (step->kind == CONCURRENCY_STEP_CANCEL) {
            size_t task = find_task(tasks, task_count, step->child);
            if (task < task_count) tasks[task].cancellation_pending = true;
        } else if (step->kind == CONCURRENCY_STEP_CHECKPOINT) {
            size_t task = find_task(tasks, task_count, step->child);
            if (task < task_count && tasks[task].cancellation_pending) {
                nodes[emitted++] = (ConcurrencyFailureNode){
                    .task = step->child,
                    .cause = CONCURRENCY_FAILURE_CANCELLATION,
                    .parent_index = SIZE_MAX,
                    .step_index = i,
                    .grade = qtt_quantity_finite(1),
                };
                tasks[task].cancellation_pending = false;
            }
        } else if (step->kind == CONCURRENCY_STEP_FAIL) {
            nodes[emitted++] = (ConcurrencyFailureNode){
                .task = step->child,
                .cause = CONCURRENCY_FAILURE_TASK,
                .parent_index = SIZE_MAX,
                .step_index = i,
                .grade = qtt_quantity_finite(1),
            };
        } else if (step->kind == CONCURRENCY_STEP_CLEANUP_FAIL) {
            nodes[emitted++] = (ConcurrencyFailureNode){
                .task = step->child,
                .cause = CONCURRENCY_FAILURE_CLEANUP,
                .parent_index = SIZE_MAX,
                .step_index = i,
                .grade = qtt_quantity_finite(1),
            };
        }
    }
    if (emitted != node_count) {
        free(tasks);
        free(nodes);
        return CONCURRENCY_FAILURE_INVALID_TRACE;
    }

    qsort(nodes, node_count, sizeof(*nodes), compare_nodes);
    size_t roots = 0;
    for (size_t i = 0; i < node_count; i++) {
        if (nodes[i].cause == CONCURRENCY_FAILURE_CLEANUP) {
            size_t primary = find_primary(nodes, node_count, nodes[i].task);
            if (primary == node_count) {
                free(tasks);
                free(nodes);
                return CONCURRENCY_FAILURE_INVALID_SHAPE;
            }
            nodes[i].parent_index = primary;
            continue;
        }
        size_t task = find_task(tasks, task_count, nodes[i].task);
        if (task == task_count) {
            free(tasks);
            free(nodes);
            return CONCURRENCY_FAILURE_INVALID_SHAPE;
        }
        ConcurrencyTaskId parent = tasks[task].parent;
        size_t parent_node = node_count;
        while (parent.module_id && parent.task_id) {
            parent_node = find_primary(nodes, node_count, parent);
            if (parent_node < node_count) break;
            size_t ancestor = find_task(tasks, task_count, parent);
            if (ancestor == task_count) break;
            parent = tasks[ancestor].parent;
        }
        if (parent_node < node_count)
            nodes[i].parent_index = parent_node;
        else
            roots++;
    }

    certificate->nodes = nodes;
    certificate->node_count = node_count;
    certificate->root_count = roots;
    certificate->total_grade = qtt_quantity_finite(node_count);
    certificate->effect_fingerprint = qtt_effect_row_fingerprint(
        authority->solver, authority->effects);
    certificate->scope_capability_id = authority->scope_capability_id;
    certificate->task_type_id = authority->task_type_id;
    certificate->fingerprint = certificate_fingerprint(certificate);
    free(tasks);
    return CONCURRENCY_FAILURE_CERTIFIED;
}

ConcurrencyFailureCertificateStatus concurrency_failure_build(
    const ConcurrencyTrace *trace,
    const ConcurrencyFailureAuthority *authority,
    ConcurrencyFailureCertificate *certificate) {
    if (!trace || !authority || !certificate)
        return CONCURRENCY_FAILURE_INVALID_ARGUMENT;
    memset(certificate, 0, sizeof(*certificate));
    return populate(trace, authority, certificate);
}

ConcurrencyFailureCertificateStatus concurrency_failure_verify(
    const ConcurrencyTrace *trace,
    const ConcurrencyFailureAuthority *authority,
    const ConcurrencyFailureCertificate *certificate) {
    if (!trace || !authority || !certificate ||
        (certificate->node_count && !certificate->nodes))
        return CONCURRENCY_FAILURE_INVALID_ARGUMENT;
    QttQuantity total = qtt_quantity_finite(0);
    for (size_t i = 0; i < certificate->node_count; i++) {
        if (!qtt_quantity_equal(
                certificate->nodes[i].grade, qtt_quantity_finite(1)))
            return CONCURRENCY_FAILURE_INVALID_GRADE;
        total = qtt_quantity_add(total, certificate->nodes[i].grade);
    }
    if (!qtt_quantity_equal(total, certificate->total_grade))
        return CONCURRENCY_FAILURE_INVALID_GRADE;
    if (certificate->fingerprint != certificate_fingerprint(certificate))
        return CONCURRENCY_FAILURE_INVALID_FINGERPRINT;

    ConcurrencyFailureCertificate expected = {0};
    ConcurrencyFailureCertificateStatus status = populate(
        trace, authority, &expected);
    if (status != CONCURRENCY_FAILURE_CERTIFIED) return status;
    bool same = certificate->node_count == expected.node_count &&
        certificate->root_count == expected.root_count &&
        qtt_quantity_equal(
            certificate->total_grade, expected.total_grade) &&
        certificate->fingerprint == expected.fingerprint;
    if (same)
        same = !certificate->node_count ||
            memcmp(certificate->nodes, expected.nodes,
                   certificate->node_count * sizeof(*certificate->nodes)) == 0;
    concurrency_failure_free(&expected);
    return same ? CONCURRENCY_FAILURE_CERTIFIED
                : CONCURRENCY_FAILURE_INVALID_SHAPE;
}

void concurrency_failure_free(
    ConcurrencyFailureCertificate *certificate) {
    if (!certificate) return;
    free(certificate->nodes);
    memset(certificate, 0, sizeof(*certificate));
}

size_t concurrency_failure_encoded_size(
    const ConcurrencyFailureCertificate *certificate) {
    if (!certificate ||
        certificate->node_count >
            (SIZE_MAX - FAILURE_WIRE_HEADER_SIZE) / FAILURE_WIRE_NODE_SIZE)
        return 0;
    return FAILURE_WIRE_HEADER_SIZE +
        certificate->node_count * FAILURE_WIRE_NODE_SIZE;
}

ConcurrencyFailureCertificateStatus concurrency_failure_encode(
    const ConcurrencyFailureCertificate *certificate,
    uint8_t *bytes, size_t capacity, size_t *written) {
    if (written) *written = 0;
    size_t required = concurrency_failure_encoded_size(certificate);
    if (!required || !bytes || !written || capacity < required ||
        (certificate->node_count && !certificate->nodes))
        return CONCURRENCY_FAILURE_INVALID_ARGUMENT;

    memcpy(bytes, failure_wire_magic, sizeof(failure_wire_magic));
    put_u16(bytes + 4, FAILURE_WIRE_VERSION);
    put_u16(bytes + 6, 0);
    put_u64(bytes + 8, certificate->node_count);
    put_u64(bytes + 16, certificate->root_count);
    put_u64(bytes + 24, certificate->total_grade.finite);
    put_u64(bytes + 32, certificate->total_grade.is_omega ? 1 : 0);
    put_u64(bytes + 40, certificate->effect_fingerprint);
    put_u64(bytes + 48, certificate->scope_capability_id);
    put_u64(bytes + 56, certificate->task_type_id);
    put_u64(bytes + 64, certificate->fingerprint);
    for (size_t i = 0; i < certificate->node_count; i++) {
        uint8_t *node_bytes = bytes + FAILURE_WIRE_HEADER_SIZE +
            i * FAILURE_WIRE_NODE_SIZE;
        const ConcurrencyFailureNode *node = &certificate->nodes[i];
        put_u64(node_bytes, node->task.module_id);
        put_u64(node_bytes + 8, node->task.task_id);
        put_u64(node_bytes + 16, (uint64_t)node->cause);
        put_u64(node_bytes + 24,
                node->parent_index == SIZE_MAX
                    ? UINT64_MAX : (uint64_t)node->parent_index);
        put_u64(node_bytes + 32, node->step_index);
        put_u64(node_bytes + 40, node->grade.finite);
        put_u64(node_bytes + 48, node->grade.is_omega ? 1 : 0);
    }
    *written = required;
    return CONCURRENCY_FAILURE_CERTIFIED;
}

ConcurrencyFailureCertificateStatus concurrency_failure_decode(
    const uint8_t *bytes, size_t byte_count,
    ConcurrencyFailureCertificate *certificate) {
    if (!bytes || !certificate)
        return CONCURRENCY_FAILURE_INVALID_ARGUMENT;
    memset(certificate, 0, sizeof(*certificate));
    if (byte_count < FAILURE_WIRE_HEADER_SIZE)
        return CONCURRENCY_FAILURE_TRUNCATED;
    if (memcmp(bytes, failure_wire_magic, sizeof(failure_wire_magic)) != 0 ||
        get_u16(bytes + 6) != 0)
        return CONCURRENCY_FAILURE_INVALID_SHAPE;
    if (get_u16(bytes + 4) != FAILURE_WIRE_VERSION)
        return CONCURRENCY_FAILURE_UNSUPPORTED_VERSION;

    uint64_t wire_count = get_u64(bytes + 8);
    if (wire_count > SIZE_MAX ||
        wire_count >
            (SIZE_MAX - FAILURE_WIRE_HEADER_SIZE) / FAILURE_WIRE_NODE_SIZE)
        return CONCURRENCY_FAILURE_INVALID_SHAPE;
    size_t node_count = (size_t)wire_count;
    size_t required = FAILURE_WIRE_HEADER_SIZE +
        node_count * FAILURE_WIRE_NODE_SIZE;
    if (byte_count < required) return CONCURRENCY_FAILURE_TRUNCATED;
    if (byte_count != required) return CONCURRENCY_FAILURE_INVALID_SHAPE;
    uint64_t wire_roots = get_u64(bytes + 16);
    if (wire_roots > node_count)
        return CONCURRENCY_FAILURE_INVALID_SHAPE;
    uint64_t total_omega = get_u64(bytes + 32);
    if (total_omega > 1)
        return CONCURRENCY_FAILURE_INVALID_GRADE;

    ConcurrencyFailureNode *nodes = node_count
        ? calloc(node_count, sizeof(*nodes)) : NULL;
    if (node_count && !nodes)
        return CONCURRENCY_FAILURE_OUT_OF_MEMORY;
    for (size_t i = 0; i < node_count; i++) {
        const uint8_t *node_bytes = bytes + FAILURE_WIRE_HEADER_SIZE +
            i * FAILURE_WIRE_NODE_SIZE;
        uint64_t cause = get_u64(node_bytes + 16);
        uint64_t parent = get_u64(node_bytes + 24);
        uint64_t step = get_u64(node_bytes + 32);
        uint64_t omega = get_u64(node_bytes + 48);
        if (cause > CONCURRENCY_FAILURE_CLEANUP || omega > 1 ||
            (parent != UINT64_MAX && parent >= node_count) ||
            step > SIZE_MAX) {
            free(nodes);
            return omega > 1 ? CONCURRENCY_FAILURE_INVALID_GRADE
                             : CONCURRENCY_FAILURE_INVALID_SHAPE;
        }
        nodes[i] = (ConcurrencyFailureNode){
            .task = {
                .module_id = get_u64(node_bytes),
                .task_id = get_u64(node_bytes + 8),
            },
            .cause = (ConcurrencyFailureCause)cause,
            .parent_index = parent == UINT64_MAX
                ? SIZE_MAX : (size_t)parent,
            .step_index = (size_t)step,
            .grade = {
                .finite = get_u64(node_bytes + 40),
                .is_omega = omega != 0,
            },
        };
    }
    *certificate = (ConcurrencyFailureCertificate){
        .nodes = nodes,
        .node_count = node_count,
        .root_count = (size_t)wire_roots,
        .total_grade = {
            .finite = get_u64(bytes + 24),
            .is_omega = total_omega != 0,
        },
        .effect_fingerprint = get_u64(bytes + 40),
        .scope_capability_id = get_u64(bytes + 48),
        .task_type_id = get_u64(bytes + 56),
        .fingerprint = get_u64(bytes + 64),
    };
    if (certificate->fingerprint != certificate_fingerprint(certificate)) {
        concurrency_failure_free(certificate);
        return CONCURRENCY_FAILURE_INVALID_FINGERPRINT;
    }
    return CONCURRENCY_FAILURE_CERTIFIED;
}
