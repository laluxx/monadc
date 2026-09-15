"""Cycle 4.3: whole-network E-Zap closure has canonical linear evidence."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


class ConcurrencyNetworkTests(unittest.TestCase):
    def test_cancellation_closure_is_complete_canonical_and_linear(self):
        source = r'''
#include "concurrency/network.h"

#include <assert.h>

static ConcurrencyNetworkEndpointId endpoint(uint64_t id) {
    return (ConcurrencyNetworkEndpointId){
        .module_id = 13,
        .endpoint_id = id,
    };
}

static ConcurrencyNetworkAuthority authority(uint64_t id) {
    return (ConcurrencyNetworkAuthority){
        .endpoint = endpoint(id),
        .place = qtt_place_root((QttCoreVar){
            .module_id = 13,
            .binder_id = id,
        }),
        .quantity = qtt_quantity_finite(1),
        .protocol_type_id = 1000 + id,
    };
}

static ConcurrencyNetworkQueueEdge edge(
    uint64_t container, uint64_t contained, uint64_t slot) {
    return (ConcurrencyNetworkQueueEdge){
        .container = endpoint(container),
        .contained = endpoint(contained),
        .slot = slot,
    };
}

int main(void) {
    ConcurrencyNetworkAuthority authorities[] = {
        authority(1), authority(2), authority(3), authority(4),
    };
    ConcurrencyNetworkQueueEdge edges[] = {
        edge(1, 2, 0),
        edge(2, 1, 0),
        edge(2, 3, 1),
    };
    ConcurrencyNetworkEndpointId roots[] = {endpoint(1)};
    ConcurrencyNetworkSnapshot snapshot = {
        .authorities = authorities,
        .authority_count = 4,
        .queue_edges = edges,
        .queue_edge_count = 3,
        .cancelled_roots = roots,
        .cancelled_root_count = 1,
    };
    ConcurrencyNetworkCertificate certificate = {0};
    assert(concurrency_network_build(&snapshot, &certificate) ==
           CONCURRENCY_NETWORK_CERTIFIED);
    assert(certificate.node_count == 3);
    assert(certificate.root_count == 1);
    assert(certificate.total_grade.finite == 3);
    assert(!certificate.total_grade.is_omega);
    assert(concurrency_network_verify(&snapshot, &certificate) ==
           CONCURRENCY_NETWORK_CERTIFIED);
    assert(certificate.nodes[0].endpoint.endpoint_id == 1);
    assert(certificate.nodes[1].endpoint.endpoint_id == 2);
    assert(certificate.nodes[2].endpoint.endpoint_id == 3);
    assert(certificate.nodes[0].parent_index == SIZE_MAX);
    assert(certificate.nodes[1].distance == 1);
    assert(certificate.nodes[2].distance == 2);

    ConcurrencyNetworkAuthority reordered_authorities[] = {
        authorities[3], authorities[1], authorities[0], authorities[2],
    };
    ConcurrencyNetworkQueueEdge reordered_edges[] = {
        edges[2], edges[1], edges[0],
    };
    ConcurrencyNetworkSnapshot reordered = snapshot;
    reordered.authorities = reordered_authorities;
    reordered.queue_edges = reordered_edges;
    ConcurrencyNetworkCertificate canonical = {0};
    assert(concurrency_network_build(&reordered, &canonical) ==
           CONCURRENCY_NETWORK_CERTIFIED);
    assert(canonical.snapshot_fingerprint ==
           certificate.snapshot_fingerprint);
    assert(canonical.fingerprint == certificate.fingerprint);
    assert(canonical.node_count == certificate.node_count);
    for (size_t i = 0; i < canonical.node_count; i++) {
        assert(canonical.nodes[i].endpoint.endpoint_id ==
               certificate.nodes[i].endpoint.endpoint_id);
        assert(canonical.nodes[i].parent_index ==
               certificate.nodes[i].parent_index);
        assert(canonical.nodes[i].distance ==
               certificate.nodes[i].distance);
    }
    concurrency_network_free(&canonical);

    uint64_t fingerprint = certificate.fingerprint;
    certificate.nodes[2].grade = qtt_quantity_omega();
    assert(concurrency_network_verify(&snapshot, &certificate) ==
           CONCURRENCY_NETWORK_INVALID_GRADE);
    certificate.nodes[2].grade = qtt_quantity_finite(1);
    certificate.nodes[2].parent_index = SIZE_MAX;
    certificate.fingerprint = fingerprint;
    assert(concurrency_network_verify(&snapshot, &certificate) !=
           CONCURRENCY_NETWORK_CERTIFIED);
    concurrency_network_free(&certificate);

    authorities[3].place = authorities[2].place;
    assert(concurrency_network_build(&snapshot, &certificate) ==
           CONCURRENCY_NETWORK_OVERLAPPING_AUTHORITY);
    authorities[3] = authority(4);

    edges[2] = edge(2, 3, 0);
    assert(concurrency_network_build(&snapshot, &certificate) ==
           CONCURRENCY_NETWORK_DUPLICATE_SLOT);
    edges[2] = edge(2, 3, 1);

    ConcurrencyNetworkQueueEdge duplicate_owner[] = {
        edge(1, 2, 0),
        edge(3, 2, 0),
    };
    snapshot.queue_edges = duplicate_owner;
    snapshot.queue_edge_count = 2;
    assert(concurrency_network_build(&snapshot, &certificate) ==
           CONCURRENCY_NETWORK_DUPLICATE_IN_FLIGHT_OWNER);

    snapshot.queue_edges = edges;
    snapshot.queue_edge_count = 3;
    roots[0] = endpoint(99);
    assert(concurrency_network_build(&snapshot, &certificate) ==
           CONCURRENCY_NETWORK_UNKNOWN_ENDPOINT);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "network_test.c"
            binary_path = Path(directory) / "network_test"
            source_path.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT / "src"), str(source_path),
                    str(ROOT / "src" / "concurrency" / "network.c"),
                    str(ROOT / "src" / "qtt" / "quantity.c"),
                    "-o", str(binary_path),
                ],
                check=True,
            )
            environment = os.environ.copy()
            environment["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(binary_path)], env=environment, check=True)


if __name__ == "__main__":
    unittest.main()
