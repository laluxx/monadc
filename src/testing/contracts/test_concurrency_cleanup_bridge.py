"""Cycle 4.7: E-Zap cancellation is discharged by structured cleanup."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


class ConcurrencyCleanupBridgeTests(unittest.TestCase):
    def test_recursive_endpoint_cancellation_requires_task_cleanup(self):
        source = r'''
#include "concurrency/deterministic.h"

#include <assert.h>

static ConcurrencyTaskId task(uint64_t id) {
    return (ConcurrencyTaskId){.module_id = 15, .task_id = id};
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
        .module_id = 15,
        .endpoint_id = id,
    };
}

static ConcurrencyNetworkInitialAuthority authority(
    uint64_t id, uint64_t protocol, ConcurrencyTaskId owner) {
    return (ConcurrencyNetworkInitialAuthority){
        .authority = {
            .endpoint = endpoint(id),
            .place = qtt_place_root((QttCoreVar){
                .module_id = 15,
                .binder_id = id,
            }),
            .quantity = qtt_quantity_finite(1),
            .protocol_type_id = protocol,
        },
        .owner = owner,
    };
}

int main(void) {
    ConcurrencySessionAction send_protocol[] = {
        action(CONCURRENCY_SESSION_SEND, 500),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencySessionAction receive_protocol[] = {
        action(CONCURRENCY_SESSION_RECEIVE, 500),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencyNetworkChannel channel = {
        .channel_id = {.module_id = 15, .channel_id = 1},
        .left = {
            .endpoint_id = 100,
            .owner = task(2),
            .quantity = qtt_quantity_finite(1),
            .protocol = send_protocol,
            .protocol_count = 2,
        },
        .right = {
            .endpoint_id = 101,
            .owner = task(3),
            .quantity = qtt_quantity_finite(1),
            .protocol = receive_protocol,
            .protocol_count = 2,
        },
        .capacity = 1,
        .scope_capability_id = 77,
        .channel_type_id = 700,
    };
    ConcurrencyNetworkInitialAuthority authorities[] = {
        authority(100, 400, task(2)),
        authority(101, 401, task(3)),
        authority(200, 500, task(2)),
    };
    ConcurrencyNetworkTraceStep network_steps[] = {
        concurrency_network_step(
            channel.channel_id,
            concurrency_channel_send_endpoint(task(2), 100, 200)),
        concurrency_network_step(
            channel.channel_id,
            concurrency_channel_cancel(task(2), 100)),
    };
    QttEffectArena *arena = qtt_effect_arena_new();
    assert(arena);
    QttEffectRow *effects = qtt_effect_empty(arena);
    QttEffectAtom cancel = concurrency_channel_effect_atom(
        CONCURRENCY_CHANNEL_EFFECT_CANCEL, 77, 700);
    QttEffectAtom send = concurrency_channel_effect_atom(
        CONCURRENCY_CHANNEL_EFFECT_SEND, 77, 700);
    QttEffectAtom receive = concurrency_channel_effect_atom(
        CONCURRENCY_CHANNEL_EFFECT_RECEIVE, 77, 700);
    effects = qtt_effect_extend_atom(arena, &cancel, effects);
    effects = qtt_effect_extend_atom(arena, &send, effects);
    effects = qtt_effect_extend_atom(arena, &receive, effects);
    assert(effects);
    QttEffectSolver *solver = qtt_effect_solver_new(arena);
    assert(solver);
    ConcurrencyNetworkTrace network = {
        .channels = &channel,
        .channel_count = 1,
        .authorities = authorities,
        .authority_count = 3,
        .steps = network_steps,
        .step_count = 2,
        .effect_solver = solver,
        .effects = effects,
    };
    ConcurrencyChannelMachineResult local = {0};
    assert(concurrency_deterministic_channel_enabled(&network, 0, &local) ==
           CONCURRENCY_DETERMINISTIC_CERTIFIED);
    assert(local.enabled);
    network_steps[0] = concurrency_network_step(
        channel.channel_id,
        concurrency_channel_receive_endpoint(task(3), 101, 200));
    assert(concurrency_deterministic_channel_enabled(&network, 0, &local) ==
           CONCURRENCY_DETERMINISTIC_CERTIFIED);
    assert(!local.enabled);
    assert(local.blocked == CONCURRENCY_CHANNEL_BLOCK_QUEUE_EMPTY);
    network_steps[0] = concurrency_network_step(
        channel.channel_id,
        concurrency_channel_send_endpoint(task(2), 100, 200));

    ConcurrencyStep cleanup_steps[] = {
        concurrency_spawn(task(1), task(2), NULL, 0),
        concurrency_cancel(task(1), task(2)),
        concurrency_checkpoint(task(2)),
        concurrency_cleanup_complete(task(2)),
        concurrency_join(task(1), task(2)),
    };
    ConcurrencyTrace cleanup = {
        .root = task(1),
        .steps = cleanup_steps,
        .step_count = 5,
    };
    ConcurrencyCleanupBridgeCertificate certificate = {0};
    assert(concurrency_cleanup_bridge_build(
               &network, &cleanup, &certificate) ==
           CONCURRENCY_CLEANUP_BRIDGE_CERTIFIED);
    assert(certificate.root_count == 1);
    assert(certificate.discharge_count == 2);
    assert(qtt_quantity_equal(
        certificate.total_grade, qtt_quantity_finite(2)));
    assert(concurrency_cleanup_bridge_verify(
               &network, &cleanup, &certificate) ==
           CONCURRENCY_CLEANUP_BRIDGE_CERTIFIED);
    assert(certificate.discharges[0].cleanup_task.task_id == 2);
    assert(certificate.discharges[1].cleanup_task.task_id == 2);

    ConcurrencyDeterministicEvent schedule[] = {
        concurrency_deterministic_task_event(0, 0),
        concurrency_deterministic_task_event(1, 1),
        concurrency_deterministic_task_event(2, 2),
        concurrency_deterministic_channel_event(0, 3),
        concurrency_deterministic_channel_event(1, 4),
        concurrency_deterministic_task_event(3, 5),
        concurrency_deterministic_task_event(4, 6),
    };
    ConcurrencyDeterministicTrace deterministic = {
        .task_trace = &cleanup,
        .network_trace = &network,
        .events = schedule,
        .event_count = 7,
    };
    ConcurrencyDeterministicEnabled enabled = {0};
    assert(concurrency_deterministic_enabled(&deterministic, 0, &enabled) ==
           CONCURRENCY_DETERMINISTIC_CERTIFIED);
    assert(enabled.task_enabled);
    assert(!enabled.channel_enabled);
    assert(enabled.channel_blocked ==
           CONCURRENCY_DETERMINISTIC_TASK_NOT_LIVE);

    assert(concurrency_deterministic_enabled(&deterministic, 3, &enabled) ==
           CONCURRENCY_DETERMINISTIC_CERTIFIED);
    assert(!enabled.task_enabled);
    assert(enabled.task_blocked ==
           CONCURRENCY_DETERMINISTIC_AWAITS_CLEANUP_DISCHARGE);
    assert(enabled.channel_enabled);

    assert(concurrency_deterministic_enabled(&deterministic, 4, &enabled) ==
           CONCURRENCY_DETERMINISTIC_CERTIFIED);
    assert(!enabled.task_enabled);
    assert(enabled.channel_enabled);

    assert(concurrency_deterministic_enabled(&deterministic, 5, &enabled) ==
           CONCURRENCY_DETERMINISTIC_CERTIFIED);
    assert(enabled.task_enabled);
    assert(!enabled.channel_enabled);
    assert(enabled.channel_blocked ==
           CONCURRENCY_DETERMINISTIC_PROJECTION_COMPLETE);
    ConcurrencyDeterministicCertificate replay = {0};
    assert(concurrency_deterministic_run(&deterministic, &replay) ==
           CONCURRENCY_DETERMINISTIC_CERTIFIED);
    assert(replay.event_count == 7);
    assert(replay.final_tick == 6);
    assert(concurrency_deterministic_verify(&deterministic, &replay) ==
           CONCURRENCY_DETERMINISTIC_CERTIFIED);
    replay.final_tick = 7;
    assert(concurrency_deterministic_verify(&deterministic, &replay) ==
           CONCURRENCY_DETERMINISTIC_INVALID_CERTIFICATE);
    schedule[4].tick = 1;
    assert(concurrency_deterministic_run(&deterministic, &replay) ==
           CONCURRENCY_DETERMINISTIC_TIME_REGRESSION);
    schedule[4].tick = 4;

    ConcurrencyDeterministicEvent before_observation[] = {
        concurrency_deterministic_task_event(0, 0),
        concurrency_deterministic_task_event(1, 1),
        concurrency_deterministic_channel_event(0, 2),
        concurrency_deterministic_channel_event(1, 3),
        concurrency_deterministic_task_event(2, 4),
        concurrency_deterministic_task_event(3, 5),
        concurrency_deterministic_task_event(4, 6),
    };
    deterministic.events = before_observation;
    assert(concurrency_deterministic_run(&deterministic, &replay) ==
           CONCURRENCY_DETERMINISTIC_CANCEL_OUTSIDE_CLEANUP);

    ConcurrencyDeterministicEvent after_cleanup[] = {
        concurrency_deterministic_task_event(0, 0),
        concurrency_deterministic_task_event(1, 1),
        concurrency_deterministic_task_event(2, 2),
        concurrency_deterministic_channel_event(0, 3),
        concurrency_deterministic_task_event(3, 4),
        concurrency_deterministic_channel_event(1, 5),
        concurrency_deterministic_task_event(4, 6),
    };
    deterministic.events = after_cleanup;
    assert(concurrency_deterministic_run(&deterministic, &replay) ==
           CONCURRENCY_DETERMINISTIC_CANCEL_OUTSIDE_CLEANUP);
    concurrency_cleanup_bridge_free(&certificate);

    ConcurrencyStep no_cleanup_steps[] = {
        concurrency_spawn(task(1), task(2), NULL, 0),
        concurrency_join(task(1), task(2)),
    };
    cleanup.steps = no_cleanup_steps;
    cleanup.step_count = 2;
    assert(concurrency_cleanup_bridge_build(
               &network, &cleanup, &certificate) ==
           CONCURRENCY_CLEANUP_BRIDGE_MISSING_CLEANUP);

    cleanup.steps = cleanup_steps;
    cleanup.step_count = 5;
    assert(concurrency_cleanup_bridge_build(
               &network, &cleanup, &certificate) ==
           CONCURRENCY_CLEANUP_BRIDGE_CERTIFIED);
    certificate.discharges[1].cleanup_task = task(3);
    assert(concurrency_cleanup_bridge_verify(
               &network, &cleanup, &certificate) ==
           CONCURRENCY_CLEANUP_BRIDGE_INVALID_DISCHARGE);
    concurrency_cleanup_bridge_free(&certificate);
    qtt_effect_solver_free(solver);
    qtt_effect_arena_free(arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "cleanup_bridge_test.c"
            binary_path = Path(directory) / "cleanup_bridge_test"
            source_path.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT / "src"), str(source_path),
                    str(ROOT / "src" / "concurrency" / "channel.c"),
                    str(ROOT / "src" / "concurrency" / "channel_effect.c"),
                    str(ROOT / "src" / "concurrency" / "channel_machine.c"),
                    str(ROOT / "src" / "concurrency" / "cleanup_bridge.c"),
                    str(ROOT / "src" / "concurrency" / "deterministic.c"),
                    str(ROOT / "src" / "concurrency" / "effect.c"),
                    str(ROOT / "src" / "concurrency" / "kernel.c"),
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
