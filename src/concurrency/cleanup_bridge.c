#include "cleanup_bridge.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool task_equal(ConcurrencyTaskId left, ConcurrencyTaskId right) {
    return left.module_id == right.module_id && left.task_id == right.task_id;
}

static bool endpoint_equal(
    ConcurrencyNetworkEndpointId left, ConcurrencyNetworkEndpointId right) {
    return left.module_id == right.module_id &&
           left.endpoint_id == right.endpoint_id;
}

static uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t certificate_fingerprint(
    const ConcurrencyCleanupBridgeCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->network_fingerprint);
    hash = mix(hash, certificate->discharge_count);
    hash = mix(hash, certificate->root_count);
    hash = mix(hash, certificate->total_grade.finite);
    hash = mix(hash, certificate->total_grade.is_omega ? 1 : 0);
    for (size_t i = 0; i < certificate->discharge_count; i++) {
        const ConcurrencyCleanupDischarge *item = &certificate->discharges[i];
        hash = mix(hash, item->endpoint.module_id);
        hash = mix(hash, item->endpoint.endpoint_id);
        hash = mix(hash, item->cleanup_task.module_id);
        hash = mix(hash, item->cleanup_task.task_id);
        hash = mix(hash, item->root_index);
        hash = mix(hash, item->grade.finite);
        hash = mix(hash, item->grade.is_omega ? 1 : 0);
    }
    return hash;
}

static bool task_discharges_cancellation(
    const ConcurrencyTrace *trace, ConcurrencyTaskId target) {
    bool pending = false;
    bool cleaning = false;
    bool complete = false;
    for (size_t i = 0; i < trace->step_count; i++) {
        const ConcurrencyStep *step = &trace->steps[i];
        if (!task_equal(step->child, target)) continue;
        switch (step->kind) {
        case CONCURRENCY_STEP_CANCEL:
            pending = true;
            break;
        case CONCURRENCY_STEP_CHECKPOINT:
            if (pending) cleaning = true;
            break;
        case CONCURRENCY_STEP_CLEANUP_COMPLETE:
            if (cleaning) complete = true;
            break;
        case CONCURRENCY_STEP_JOIN:
            if (complete) return true;
            break;
        default:
            break;
        }
    }
    return false;
}

static bool root_actor(
    const ConcurrencyNetworkTrace *trace,
    ConcurrencyNetworkEndpointId root,
    ConcurrencyTaskId *actor) {
    bool found = false;
    for (size_t i = 0; i < trace->step_count; i++) {
        const ConcurrencyNetworkTraceStep *event = &trace->steps[i];
        if (event->channel_step.kind != CONCURRENCY_CHANNEL_STEP_CANCEL)
            continue;
        ConcurrencyNetworkEndpointId endpoint = {
            .module_id = event->channel_id.module_id,
            .endpoint_id = event->channel_step.endpoint_id,
        };
        if (!endpoint_equal(endpoint, root)) continue;
        if (found && !task_equal(*actor, event->channel_step.actor))
            return false;
        *actor = event->channel_step.actor;
        found = true;
    }
    return found;
}

void concurrency_cleanup_bridge_free(
    ConcurrencyCleanupBridgeCertificate *certificate) {
    if (!certificate) return;
    free(certificate->discharges);
    memset(certificate, 0, sizeof(*certificate));
}

ConcurrencyCleanupBridgeStatus concurrency_cleanup_bridge_build(
    const ConcurrencyNetworkTrace *network_trace,
    const ConcurrencyTrace *task_trace,
    ConcurrencyCleanupBridgeCertificate *certificate) {
    if (!network_trace || !task_trace || !certificate)
        return CONCURRENCY_CLEANUP_BRIDGE_INVALID_ARGUMENT;
    memset(certificate, 0, sizeof(*certificate));
    if (concurrency_verify_trace(task_trace).error != CONCURRENCY_VALID)
        return CONCURRENCY_CLEANUP_BRIDGE_INVALID_TASK_TRACE;

    ConcurrencyNetworkDerived network = {0};
    if (concurrency_network_derive(network_trace, &network) !=
        CONCURRENCY_NETWORK_TRACE_CERTIFIED)
        return CONCURRENCY_CLEANUP_BRIDGE_INVALID_NETWORK;
    size_t count = network.certificate.node_count;
    ConcurrencyCleanupDischarge *discharges =
        calloc(count, sizeof(*discharges));
    if (count && !discharges) {
        concurrency_network_derived_free(&network);
        return CONCURRENCY_CLEANUP_BRIDGE_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < count; i++) {
        size_t root = i;
        while (network.certificate.nodes[root].parent_index != SIZE_MAX)
            root = network.certificate.nodes[root].parent_index;
        ConcurrencyTaskId actor = {0};
        if (!root_actor(
                network_trace,
                network.certificate.nodes[root].endpoint, &actor) ||
            !task_discharges_cancellation(task_trace, actor)) {
            free(discharges);
            concurrency_network_derived_free(&network);
            return CONCURRENCY_CLEANUP_BRIDGE_MISSING_CLEANUP;
        }
        discharges[i] = (ConcurrencyCleanupDischarge){
            .endpoint = network.certificate.nodes[i].endpoint,
            .cleanup_task = actor,
            .root_index = root,
            .grade = qtt_quantity_finite(1),
        };
    }
    certificate->discharges = discharges;
    certificate->discharge_count = count;
    certificate->root_count = network.certificate.root_count;
    certificate->total_grade = network.certificate.total_grade;
    certificate->network_fingerprint = network.certificate.fingerprint;
    certificate->fingerprint = certificate_fingerprint(certificate);
    concurrency_network_derived_free(&network);
    return CONCURRENCY_CLEANUP_BRIDGE_CERTIFIED;
}

ConcurrencyCleanupBridgeStatus concurrency_cleanup_bridge_verify(
    const ConcurrencyNetworkTrace *network_trace,
    const ConcurrencyTrace *task_trace,
    const ConcurrencyCleanupBridgeCertificate *certificate) {
    if (!network_trace || !task_trace || !certificate ||
        (certificate->discharge_count && !certificate->discharges))
        return CONCURRENCY_CLEANUP_BRIDGE_INVALID_ARGUMENT;
    ConcurrencyCleanupBridgeCertificate expected = {0};
    ConcurrencyCleanupBridgeStatus status = concurrency_cleanup_bridge_build(
        network_trace, task_trace, &expected);
    if (status != CONCURRENCY_CLEANUP_BRIDGE_CERTIFIED) return status;
    bool equal = certificate->discharge_count == expected.discharge_count &&
        certificate->root_count == expected.root_count &&
        qtt_quantity_equal(certificate->total_grade, expected.total_grade) &&
        certificate->network_fingerprint == expected.network_fingerprint &&
        certificate->fingerprint == certificate_fingerprint(certificate) &&
        certificate->fingerprint == expected.fingerprint;
    for (size_t i = 0; equal && i < certificate->discharge_count; i++) {
        const ConcurrencyCleanupDischarge *left = &certificate->discharges[i];
        const ConcurrencyCleanupDischarge *right = &expected.discharges[i];
        equal = endpoint_equal(left->endpoint, right->endpoint) &&
            task_equal(left->cleanup_task, right->cleanup_task) &&
            left->root_index == right->root_index &&
            qtt_quantity_equal(left->grade, right->grade);
    }
    concurrency_cleanup_bridge_free(&expected);
    return equal ? CONCURRENCY_CLEANUP_BRIDGE_CERTIFIED
                 : CONCURRENCY_CLEANUP_BRIDGE_INVALID_DISCHARGE;
}
