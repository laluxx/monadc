#include "network.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool endpoint_equal(
    ConcurrencyNetworkEndpointId left,
    ConcurrencyNetworkEndpointId right) {
    return left.module_id == right.module_id &&
           left.endpoint_id == right.endpoint_id;
}

static bool endpoint_less(
    ConcurrencyNetworkEndpointId left,
    ConcurrencyNetworkEndpointId right) {
    return left.module_id < right.module_id ||
        (left.module_id == right.module_id &&
         left.endpoint_id < right.endpoint_id);
}

static bool endpoint_valid(ConcurrencyNetworkEndpointId endpoint) {
    return endpoint.module_id != 0 && endpoint.endpoint_id != 0;
}

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

static size_t find_authority(
    const ConcurrencyNetworkSnapshot *snapshot,
    ConcurrencyNetworkEndpointId endpoint) {
    for (size_t i = 0; i < snapshot->authority_count; i++)
        if (endpoint_equal(snapshot->authorities[i].endpoint, endpoint))
            return i;
    return snapshot->authority_count;
}

static bool is_root(
    const ConcurrencyNetworkSnapshot *snapshot,
    ConcurrencyNetworkEndpointId endpoint) {
    for (size_t i = 0; i < snapshot->cancelled_root_count; i++)
        if (endpoint_equal(snapshot->cancelled_roots[i], endpoint))
            return true;
    return false;
}

static uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t endpoint_hash(ConcurrencyNetworkEndpointId endpoint) {
    return mix(mix(UINT64_C(1469598103934665603), endpoint.module_id),
               endpoint.endpoint_id);
}

static uint64_t snapshot_fingerprint(
    const ConcurrencyNetworkSnapshot *snapshot) {
    uint64_t authority_sum = 0, authority_xor = 0;
    uint64_t edge_sum = 0, edge_xor = 0;
    uint64_t root_sum = 0, root_xor = 0;
    for (size_t i = 0; i < snapshot->authority_count; i++) {
        const ConcurrencyNetworkAuthority *authority =
            &snapshot->authorities[i];
        uint64_t item = endpoint_hash(authority->endpoint);
        item = mix(item, authority->protocol_type_id);
        item = mix(item, authority->place.root.module_id);
        item = mix(item, authority->place.root.binder_id);
        item = mix(item, authority->place.projection_id);
        item = mix(item, authority->place.projection_depth);
        for (uint8_t depth = 0;
             depth < authority->place.projection_depth; depth++)
            item = mix(item, authority->place.projection_path[depth]);
        item = mix(item, authority->quantity.finite);
        item = mix(item, authority->quantity.is_omega ? 1 : 0);
        authority_sum += item;
        authority_xor ^= item;
    }
    for (size_t i = 0; i < snapshot->queue_edge_count; i++) {
        const ConcurrencyNetworkQueueEdge *edge = &snapshot->queue_edges[i];
        uint64_t item = mix(endpoint_hash(edge->container),
                            endpoint_hash(edge->contained));
        item = mix(item, edge->slot);
        edge_sum += item;
        edge_xor ^= item;
    }
    for (size_t i = 0; i < snapshot->cancelled_root_count; i++) {
        uint64_t item = endpoint_hash(snapshot->cancelled_roots[i]);
        root_sum += item;
        root_xor ^= item;
    }
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, snapshot->authority_count);
    hash = mix(hash, authority_sum);
    hash = mix(hash, authority_xor);
    hash = mix(hash, snapshot->queue_edge_count);
    hash = mix(hash, edge_sum);
    hash = mix(hash, edge_xor);
    hash = mix(hash, snapshot->cancelled_root_count);
    hash = mix(hash, root_sum);
    return mix(hash, root_xor);
}

static uint64_t certificate_fingerprint(
    const ConcurrencyNetworkCertificate *certificate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, certificate->node_count);
    hash = mix(hash, certificate->root_count);
    hash = mix(hash, certificate->total_grade.finite);
    hash = mix(hash, certificate->total_grade.is_omega ? 1 : 0);
    hash = mix(hash, certificate->snapshot_fingerprint);
    for (size_t i = 0; i < certificate->node_count; i++) {
        const ConcurrencyNetworkCancellationNode *node =
            &certificate->nodes[i];
        hash = mix(hash, node->endpoint.module_id);
        hash = mix(hash, node->endpoint.endpoint_id);
        hash = mix(hash, node->parent_index);
        hash = mix(hash, node->distance);
        hash = mix(hash, node->grade.finite);
        hash = mix(hash, node->grade.is_omega ? 1 : 0);
    }
    return hash;
}

static ConcurrencyNetworkStatus validate_snapshot(
    const ConcurrencyNetworkSnapshot *snapshot) {
    if (!snapshot || !snapshot->authority_count ||
        !snapshot->authorities || !snapshot->cancelled_root_count ||
        !snapshot->cancelled_roots ||
        (snapshot->queue_edge_count && !snapshot->queue_edges))
        return CONCURRENCY_NETWORK_INVALID_ARGUMENT;
    for (size_t i = 0; i < snapshot->authority_count; i++) {
        const ConcurrencyNetworkAuthority *authority =
            &snapshot->authorities[i];
        if (!endpoint_valid(authority->endpoint) ||
            !authority->protocol_type_id || !place_valid(authority->place))
            return CONCURRENCY_NETWORK_MALFORMED;
        if (!qtt_quantity_equal(
                authority->quantity, qtt_quantity_finite(1)))
            return CONCURRENCY_NETWORK_NON_LINEAR_AUTHORITY;
        for (size_t j = 0; j < i; j++)
            if (endpoint_equal(
                    snapshot->authorities[j].endpoint,
                    authority->endpoint) ||
                qtt_place_overlaps(
                    snapshot->authorities[j].place, authority->place))
                return CONCURRENCY_NETWORK_OVERLAPPING_AUTHORITY;
    }
    for (size_t i = 0; i < snapshot->cancelled_root_count; i++) {
        if (find_authority(snapshot, snapshot->cancelled_roots[i]) ==
            snapshot->authority_count)
            return CONCURRENCY_NETWORK_UNKNOWN_ENDPOINT;
        for (size_t j = 0; j < i; j++)
            if (endpoint_equal(snapshot->cancelled_roots[j],
                               snapshot->cancelled_roots[i]))
                return CONCURRENCY_NETWORK_MALFORMED;
    }
    for (size_t i = 0; i < snapshot->queue_edge_count; i++) {
        const ConcurrencyNetworkQueueEdge *edge = &snapshot->queue_edges[i];
        if (find_authority(snapshot, edge->container) ==
                snapshot->authority_count ||
            find_authority(snapshot, edge->contained) ==
                snapshot->authority_count)
            return CONCURRENCY_NETWORK_UNKNOWN_ENDPOINT;
        for (size_t j = 0; j < i; j++) {
            const ConcurrencyNetworkQueueEdge *prior =
                &snapshot->queue_edges[j];
            if (endpoint_equal(prior->container, edge->container) &&
                prior->slot == edge->slot)
                return CONCURRENCY_NETWORK_DUPLICATE_SLOT;
            if (endpoint_equal(prior->contained, edge->contained))
                return CONCURRENCY_NETWORK_DUPLICATE_IN_FLIGHT_OWNER;
        }
    }
    return CONCURRENCY_NETWORK_CERTIFIED;
}

static int compare_nodes(const void *left_ptr, const void *right_ptr) {
    const ConcurrencyNetworkCancellationNode *left = left_ptr;
    const ConcurrencyNetworkCancellationNode *right = right_ptr;
    if (endpoint_equal(left->endpoint, right->endpoint)) return 0;
    return endpoint_less(left->endpoint, right->endpoint) ? -1 : 1;
}

static size_t find_node(
    const ConcurrencyNetworkCancellationNode *nodes, size_t count,
    ConcurrencyNetworkEndpointId endpoint) {
    for (size_t i = 0; i < count; i++)
        if (endpoint_equal(nodes[i].endpoint, endpoint)) return i;
    return count;
}

static ConcurrencyNetworkStatus populate(
    const ConcurrencyNetworkSnapshot *snapshot,
    ConcurrencyNetworkCertificate *certificate) {
    ConcurrencyNetworkStatus status = validate_snapshot(snapshot);
    if (status != CONCURRENCY_NETWORK_CERTIFIED) return status;
    size_t *distance = malloc(snapshot->authority_count * sizeof(*distance));
    if (!distance) return CONCURRENCY_NETWORK_OUT_OF_MEMORY;
    for (size_t i = 0; i < snapshot->authority_count; i++)
        distance[i] = SIZE_MAX;
    for (size_t i = 0; i < snapshot->cancelled_root_count; i++)
        distance[find_authority(snapshot, snapshot->cancelled_roots[i])] = 0;

    bool changed = true;
    for (size_t round = 0; round < snapshot->authority_count && changed;
         round++) {
        changed = false;
        for (size_t i = 0; i < snapshot->queue_edge_count; i++) {
            size_t from = find_authority(
                snapshot, snapshot->queue_edges[i].container);
            size_t to = find_authority(
                snapshot, snapshot->queue_edges[i].contained);
            if (distance[from] != SIZE_MAX &&
                distance[to] > distance[from] + 1) {
                distance[to] = distance[from] + 1;
                changed = true;
            }
        }
    }
    size_t node_count = 0;
    for (size_t i = 0; i < snapshot->authority_count; i++)
        if (distance[i] != SIZE_MAX) node_count++;
    ConcurrencyNetworkCancellationNode *nodes =
        calloc(node_count, sizeof(*nodes));
    if (!nodes) {
        free(distance);
        return CONCURRENCY_NETWORK_OUT_OF_MEMORY;
    }
    size_t emitted = 0;
    for (size_t i = 0; i < snapshot->authority_count; i++) {
        if (distance[i] == SIZE_MAX) continue;
        nodes[emitted++] = (ConcurrencyNetworkCancellationNode){
            .endpoint = snapshot->authorities[i].endpoint,
            .parent_index = SIZE_MAX,
            .distance = distance[i],
            .grade = qtt_quantity_finite(1),
        };
    }
    qsort(nodes, node_count, sizeof(*nodes), compare_nodes);
    for (size_t i = 0; i < node_count; i++) {
        if (is_root(snapshot, nodes[i].endpoint)) continue;
        bool found = false;
        ConcurrencyNetworkEndpointId parent = {0};
        for (size_t j = 0; j < snapshot->queue_edge_count; j++) {
            const ConcurrencyNetworkQueueEdge *edge =
                &snapshot->queue_edges[j];
            if (!endpoint_equal(edge->contained, nodes[i].endpoint))
                continue;
            size_t candidate = find_authority(snapshot, edge->container);
            if (distance[candidate] == SIZE_MAX ||
                distance[candidate] + 1 != nodes[i].distance)
                continue;
            if (!found || endpoint_less(edge->container, parent)) {
                parent = edge->container;
                found = true;
            }
        }
        if (!found) {
            free(nodes);
            free(distance);
            return CONCURRENCY_NETWORK_INVALID_SHAPE;
        }
        nodes[i].parent_index = find_node(nodes, node_count, parent);
        if (nodes[i].parent_index == node_count) {
            free(nodes);
            free(distance);
            return CONCURRENCY_NETWORK_INVALID_SHAPE;
        }
    }
    certificate->nodes = nodes;
    certificate->node_count = node_count;
    certificate->root_count = snapshot->cancelled_root_count;
    certificate->total_grade = qtt_quantity_finite(node_count);
    certificate->snapshot_fingerprint = snapshot_fingerprint(snapshot);
    certificate->fingerprint = certificate_fingerprint(certificate);
    free(distance);
    return CONCURRENCY_NETWORK_CERTIFIED;
}

ConcurrencyNetworkStatus concurrency_network_build(
    const ConcurrencyNetworkSnapshot *snapshot,
    ConcurrencyNetworkCertificate *certificate) {
    if (!snapshot || !certificate)
        return CONCURRENCY_NETWORK_INVALID_ARGUMENT;
    memset(certificate, 0, sizeof(*certificate));
    return populate(snapshot, certificate);
}

ConcurrencyNetworkStatus concurrency_network_verify(
    const ConcurrencyNetworkSnapshot *snapshot,
    const ConcurrencyNetworkCertificate *certificate) {
    if (!snapshot || !certificate ||
        (certificate->node_count && !certificate->nodes))
        return CONCURRENCY_NETWORK_INVALID_ARGUMENT;
    QttQuantity total = qtt_quantity_finite(0);
    for (size_t i = 0; i < certificate->node_count; i++) {
        if (!qtt_quantity_equal(
                certificate->nodes[i].grade, qtt_quantity_finite(1)))
            return CONCURRENCY_NETWORK_INVALID_GRADE;
        total = qtt_quantity_add(total, certificate->nodes[i].grade);
    }
    if (!qtt_quantity_equal(total, certificate->total_grade))
        return CONCURRENCY_NETWORK_INVALID_GRADE;
    if (certificate->fingerprint != certificate_fingerprint(certificate))
        return CONCURRENCY_NETWORK_INVALID_FINGERPRINT;

    ConcurrencyNetworkCertificate expected = {0};
    ConcurrencyNetworkStatus status = populate(snapshot, &expected);
    if (status != CONCURRENCY_NETWORK_CERTIFIED) return status;
    bool same = certificate->node_count == expected.node_count &&
        certificate->root_count == expected.root_count &&
        qtt_quantity_equal(certificate->total_grade, expected.total_grade) &&
        certificate->snapshot_fingerprint == expected.snapshot_fingerprint &&
        certificate->fingerprint == expected.fingerprint;
    for (size_t i = 0; same && i < certificate->node_count; i++) {
        const ConcurrencyNetworkCancellationNode *left =
            &certificate->nodes[i];
        const ConcurrencyNetworkCancellationNode *right =
            &expected.nodes[i];
        same = endpoint_equal(left->endpoint, right->endpoint) &&
            left->parent_index == right->parent_index &&
            left->distance == right->distance &&
            qtt_quantity_equal(left->grade, right->grade);
    }
    concurrency_network_free(&expected);
    return same ? CONCURRENCY_NETWORK_CERTIFIED
                : CONCURRENCY_NETWORK_INVALID_SHAPE;
}

void concurrency_network_free(
    ConcurrencyNetworkCertificate *certificate) {
    if (!certificate) return;
    free(certificate->nodes);
    memset(certificate, 0, sizeof(*certificate));
}
