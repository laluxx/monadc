"""Cycle 4.6: global protocol replay requires scoped QTT effects."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


class ConcurrencyNetworkTraceTests(unittest.TestCase):
    def test_interleaved_channels_derive_the_authority_graph(self):
        source = r'''
#include "concurrency/deterministic.h"

#include <assert.h>

static ConcurrencyTaskId task(uint64_t id) {
    return (ConcurrencyTaskId){.module_id = 14, .task_id = id};
}

static ConcurrencySessionAction action(
    ConcurrencySessionActionKind kind, uint64_t payload) {
    return (ConcurrencySessionAction){
        .kind = kind,
        .payload_type_id = payload,
    };
}

static ConcurrencyNetworkEndpointId endpoint(uint64_t id) {
    return (ConcurrencyNetworkEndpointId){
        .module_id = 14,
        .endpoint_id = id,
    };
}

static ConcurrencyNetworkInitialAuthority authority(
    uint64_t id, uint64_t protocol, ConcurrencyTaskId owner) {
    return (ConcurrencyNetworkInitialAuthority){
        .authority = {
            .endpoint = endpoint(id),
            .place = qtt_place_root((QttCoreVar){
                .module_id = 14,
                .binder_id = id,
            }),
            .quantity = qtt_quantity_finite(1),
            .protocol_type_id = protocol,
        },
        .owner = owner,
    };
}

static ConcurrencyChannelEndpoint channel_endpoint(
    uint64_t id, ConcurrencyTaskId owner,
    const ConcurrencySessionAction *protocol, size_t count) {
    return (ConcurrencyChannelEndpoint){
        .endpoint_id = id,
        .owner = owner,
        .quantity = qtt_quantity_finite(1),
        .protocol = protocol,
        .protocol_count = count,
    };
}

int main(void) {
    ConcurrencySessionAction outer_send[] = {
        action(CONCURRENCY_SESSION_SEND, 1200),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencySessionAction outer_receive[] = {
        action(CONCURRENCY_SESSION_RECEIVE, 1200),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencySessionAction inner_send[] = {
        action(CONCURRENCY_SESSION_SEND, 1300),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencySessionAction inner_receive[] = {
        action(CONCURRENCY_SESSION_RECEIVE, 1300),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencyNetworkChannel channels[] = {
        {
            .channel_id = {.module_id = 14, .channel_id = 1},
            .left = channel_endpoint(100, task(1), outer_send, 2),
            .right = channel_endpoint(101, task(2), outer_receive, 2),
            .capacity = 1,
            .scope_capability_id = 77,
            .channel_type_id = 9001,
        },
        {
            .channel_id = {.module_id = 14, .channel_id = 2},
            .left = channel_endpoint(200, task(1), inner_send, 2),
            .right = channel_endpoint(201, task(2), inner_receive, 2),
            .capacity = 1,
            .scope_capability_id = 77,
            .channel_type_id = 9002,
        },
    };
    ConcurrencyNetworkInitialAuthority authorities[] = {
        authority(100, 1100, task(1)),
        authority(101, 1101, task(2)),
        authority(200, 1200, task(1)),
        authority(201, 1201, task(2)),
        authority(300, 1300, task(1)),
    };
    ConcurrencyNetworkTraceStep steps[] = {
        concurrency_network_step(
            channels[1].channel_id,
            concurrency_channel_send_endpoint(task(1), 200, 300)),
        concurrency_network_step(
            channels[0].channel_id,
            concurrency_channel_send_endpoint(task(1), 100, 200)),
        concurrency_network_step(
            channels[0].channel_id,
            concurrency_channel_cancel(task(1), 100)),
    };
    ConcurrencyNetworkTrace trace = {
        .channels = channels,
        .channel_count = 2,
        .authorities = authorities,
        .authority_count = 5,
        .steps = steps,
        .step_count = 3,
    };
    QttEffectArena *arena = qtt_effect_arena_new();
    assert(arena);
    QttEffectRow *row = qtt_effect_empty(arena);
    for (size_t channel = 0; channel < 2; channel++)
        for (int operation = CONCURRENCY_CHANNEL_EFFECT_CANCEL;
             operation >= CONCURRENCY_CHANNEL_EFFECT_SEND; operation--) {
            QttEffectAtom atom = concurrency_channel_effect_atom(
                (ConcurrencyChannelEffectOperation)operation,
                channels[channel].scope_capability_id,
                channels[channel].channel_type_id);
            row = qtt_effect_extend_atom(arena, &atom, row);
            assert(row);
        }
    QttEffectSolver *solver = qtt_effect_solver_new(arena);
    assert(solver);
    trace.effect_solver = solver;
    trace.effects = row;
    ConcurrencyNetworkDerived derived = {0};
    assert(concurrency_network_derive(&trace, &derived) ==
           CONCURRENCY_NETWORK_TRACE_CERTIFIED);
    assert(derived.snapshot.queue_edge_count == 2);
    assert(derived.snapshot.cancelled_root_count == 1);
    assert(derived.certificate.node_count == 3);
    assert(concurrency_network_verify(
               &derived.snapshot, &derived.certificate) ==
           CONCURRENCY_NETWORK_CERTIFIED);
    assert(derived.certificate.nodes[0].endpoint.endpoint_id == 100);
    assert(derived.certificate.nodes[1].endpoint.endpoint_id == 200);
    assert(derived.certificate.nodes[2].endpoint.endpoint_id == 300);
    concurrency_network_derived_free(&derived);

    channels[0].scope_capability_id = 78;
    assert(concurrency_network_derive(&trace, &derived) ==
           CONCURRENCY_NETWORK_TRACE_EFFECT_ERROR);
    channels[0].scope_capability_id = 77;

    ConcurrencyNetworkTraceStep reordered[] = {
        steps[1], steps[0], steps[2],
    };
    trace.steps = reordered;
    assert(concurrency_network_derive(&trace, &derived) ==
           CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR);

    trace.steps = steps;
    inner_send[0].payload_type_id = 1301;
    assert(concurrency_network_derive(&trace, &derived) ==
           CONCURRENCY_NETWORK_TRACE_PROTOCOL_ERROR);
    inner_send[0].payload_type_id = 1300;

    channels[0].left.owner = task(9);
    assert(concurrency_network_derive(&trace, &derived) ==
           CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR);
    channels[0].left.owner = task(1);

    ConcurrencyNetworkTraceStep received[] = {
        concurrency_network_step(
            channels[1].channel_id,
            concurrency_channel_send_endpoint(task(1), 200, 300)),
        concurrency_network_step(
            channels[1].channel_id,
            concurrency_channel_receive_endpoint(task(2), 201, 300)),
        concurrency_network_step(
            channels[1].channel_id,
            concurrency_channel_cancel(task(1), 200)),
    };
    trace.steps = received;
    assert(concurrency_network_derive(&trace, &derived) ==
           CONCURRENCY_NETWORK_TRACE_CERTIFIED);
    assert(derived.snapshot.queue_edge_count == 0);
    assert(derived.certificate.node_count == 1);
    assert(derived.certificate.nodes[0].endpoint.endpoint_id == 200);
    concurrency_network_derived_free(&derived);
    received[1] = concurrency_network_step(
        channels[1].channel_id,
        concurrency_channel_receive_endpoint(task(2), 201, 200));
    assert(concurrency_network_derive(&trace, &derived) ==
           CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR);
    received[1] = concurrency_network_step(
        channels[1].channel_id,
        concurrency_channel_receive_endpoint(task(2), 201, 300));

    ConcurrencyNetworkTraceStep buffered_transfer[] = {
        concurrency_network_step(
            channels[0].channel_id,
            concurrency_channel_send_endpoint(task(1), 100, 200)),
        concurrency_network_step(
            channels[0].channel_id,
            concurrency_channel_receive_endpoint(task(2), 101, 200)),
        concurrency_network_step(
            channels[1].channel_id,
            concurrency_channel_cancel(task(2), 200)),
    };
    trace.steps = buffered_transfer;
    trace.step_count = 3;
    ConcurrencyChannelMachineResult readiness = {0};
    assert(concurrency_deterministic_channel_enabled(
               &trace, 2, &readiness) ==
           CONCURRENCY_DETERMINISTIC_CERTIFIED);
    assert(readiness.enabled);
    buffered_transfer[1] = concurrency_network_step(
        channels[0].channel_id,
        concurrency_channel_receive_endpoint(task(2), 100, 200));
    assert(concurrency_deterministic_channel_enabled(
               &trace, 2, &readiness) ==
           CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION);
    buffered_transfer[1] = concurrency_network_step(
        channels[0].channel_id,
        concurrency_channel_receive_endpoint(task(2), 101, 200));

    channels[0].capacity = 0;
    ConcurrencyNetworkTraceStep rendezvous[] = {
        concurrency_network_step(
            channels[0].channel_id,
            concurrency_channel_rendezvous_endpoint(
                task(1), 100, task(2), 101, 200)),
        concurrency_network_step(
            channels[1].channel_id,
            concurrency_channel_cancel(task(2), 200)),
    };
    trace.steps = rendezvous;
    trace.step_count = 2;
    assert(concurrency_network_derive(&trace, &derived) ==
           CONCURRENCY_NETWORK_TRACE_CERTIFIED);
    assert(derived.snapshot.queue_edge_count == 0);
    assert(derived.certificate.node_count == 1);
    assert(derived.certificate.nodes[0].endpoint.endpoint_id == 200);
    concurrency_network_derived_free(&derived);
    readiness = (ConcurrencyChannelMachineResult){0};
    assert(concurrency_deterministic_channel_enabled(
               &trace, 1, &readiness) ==
           CONCURRENCY_DETERMINISTIC_CERTIFIED);
    assert(readiness.enabled);

    rendezvous[0] = concurrency_network_step(
        channels[0].channel_id,
        concurrency_channel_rendezvous_endpoint(
            task(1), 100, task(9), 101, 200));
    assert(concurrency_network_derive(&trace, &derived) ==
           CONCURRENCY_NETWORK_TRACE_AUTHORITY_ERROR);
    rendezvous[0] = concurrency_network_step(
        channels[0].channel_id,
        concurrency_channel_rendezvous_endpoint(
            task(1), 100, task(2), 101, 200));

    QttEffectAtom send = concurrency_channel_effect_atom(
        CONCURRENCY_CHANNEL_EFFECT_SEND, 77, 9002);
    QttEffectRow *send_only = qtt_effect_extend_atom(
        arena, &send, qtt_effect_empty(arena));
    trace.effects = send_only;
    assert(concurrency_network_derive(&trace, &derived) ==
           CONCURRENCY_NETWORK_TRACE_EFFECT_ERROR);
    qtt_effect_solver_free(solver);
    qtt_effect_arena_free(arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "network_trace_test.c"
            binary_path = Path(directory) / "network_trace_test"
            source_path.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT / "src"), str(source_path),
                    str(ROOT / "src" / "concurrency" / "channel.c"),
                    str(ROOT / "src" / "concurrency" / "channel_machine.c"),
                    str(ROOT / "src" / "concurrency" / "channel_effect.c"),
                    str(ROOT / "src" / "concurrency" / "cleanup_bridge.c"),
                    str(ROOT / "src" / "concurrency" / "deterministic.c"),
                    str(ROOT / "src" / "concurrency" / "kernel.c"),
                    str(ROOT / "src" / "concurrency" / "effect.c"),
                    str(ROOT / "src" / "concurrency" / "network.c"),
                    str(ROOT / "src" / "concurrency" / "network_trace.c"),
                    str(ROOT / "src" / "effects" / "effect.c"),
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
