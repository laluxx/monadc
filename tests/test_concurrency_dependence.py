"""Cycle 5.6: semantic event footprints define DPOR dependence."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ConcurrencyDependenceTests(unittest.TestCase):
    def test_task_channel_authority_and_cleanup_conflicts(self):
        source = r'''
#include "concurrency/dependence.h"

#include <assert.h>

static ConcurrencyTaskId task(uint64_t id) {
    return (ConcurrencyTaskId){.module_id = 21, .task_id = id};
}

static ConcurrencyNetworkEndpointId endpoint(uint64_t id) {
    return (ConcurrencyNetworkEndpointId){
        .module_id = 21, .endpoint_id = id,
    };
}

static ConcurrencyNetworkInitialAuthority authority(
    uint64_t id, uint64_t binder, ConcurrencyTaskId owner) {
    return (ConcurrencyNetworkInitialAuthority){
        .authority = {
            .endpoint = endpoint(id),
            .place = qtt_place_root((QttCoreVar){
                .module_id = 21, .binder_id = binder,
            }),
            .quantity = qtt_quantity_finite(1),
            .protocol_type_id = id + 1000,
        },
        .owner = owner,
    };
}

static void expect(
    const ConcurrencyDeterministicTrace *trace,
    ConcurrencyDeterministicEvent left,
    ConcurrencyDeterministicEvent right, bool dependent,
    uint32_t required_reason) {
    ConcurrencyDependence result = {0};
    assert(concurrency_deterministic_dependence(
               trace, left, right, &result) ==
           CONCURRENCY_DETERMINISTIC_CERTIFIED);
    assert(result.dependent == dependent);
    assert((result.reasons & required_reason) == required_reason);
    ConcurrencyDependence reverse = {0};
    assert(concurrency_deterministic_dependence(
               trace, right, left, &reverse) ==
           CONCURRENCY_DETERMINISTIC_CERTIFIED);
    assert(reverse.dependent == result.dependent);
    assert(reverse.reasons == result.reasons);
}

int main(void) {
    ConcurrencyNetworkChannel channels[] = {
        {.channel_id = {.module_id = 21, .channel_id = 1},
         .scope_capability_id = 71, .channel_type_id = 701},
        {.channel_id = {.module_id = 21, .channel_id = 2},
         .scope_capability_id = 72, .channel_type_id = 702},
        {.channel_id = {.module_id = 21, .channel_id = 3},
         .scope_capability_id = 73, .channel_type_id = 703},
    };
    ConcurrencyNetworkInitialAuthority authorities[] = {
        authority(100, 100, task(1)), authority(101, 101, task(2)),
        authority(200, 200, task(3)), authority(201, 201, task(4)),
        authority(300, 300, task(5)), authority(301, 301, task(6)),
        authority(900, 900, task(1)),
    };
    ConcurrencyNetworkTraceStep network_steps[] = {
        concurrency_network_step(
            channels[0].channel_id,
            concurrency_channel_send(task(1), 100, 7)),
        concurrency_network_step(
            channels[1].channel_id,
            concurrency_channel_send(task(3), 200, 8)),
        concurrency_network_step(
            channels[0].channel_id,
            concurrency_channel_receive(task(2), 101, 7)),
        concurrency_network_step(
            channels[2].channel_id,
            concurrency_channel_send(task(1), 300, 9)),
        concurrency_network_step(
            channels[1].channel_id,
            concurrency_channel_send_endpoint(task(3), 200, 900)),
        concurrency_network_step(
            channels[2].channel_id,
            concurrency_channel_send_endpoint(task(5), 300, 900)),
        concurrency_network_step(
            channels[1].channel_id,
            concurrency_channel_cancel(task(3), 200)),
    };
    ConcurrencyNetworkTrace network = {
        .channels = channels,
        .channel_count = 3,
        .authorities = authorities,
        .authority_count = 7,
        .steps = network_steps,
        .step_count = 7,
    };
    QttPlace captured = qtt_place_root((QttCoreVar){
        .module_id = 21, .binder_id = 900,
    });
    ConcurrencyStep task_steps[] = {
        concurrency_spawn(task(1), task(3), NULL, 0),
        concurrency_cleanup_complete(task(3)),
        concurrency_spawn(task(7), task(8), &captured, 1),
    };
    ConcurrencyTrace tasks = {
        .root = task(1), .steps = task_steps, .step_count = 3,
    };
    ConcurrencyDeterministicTrace trace = {
        .task_trace = &tasks, .network_trace = &network,
    };

    ConcurrencyDeterministicEvent channel0 =
        concurrency_deterministic_channel_event(0, 0);
    ConcurrencyDeterministicEvent channel1 =
        concurrency_deterministic_channel_event(1, 0);
    expect(&trace, channel0, channel1, false, CONCURRENCY_DEPENDENCE_NONE);
    expect(&trace, channel0,
           concurrency_deterministic_channel_event(2, 0), true,
           CONCURRENCY_DEPENDENCE_CHANNEL);
    expect(&trace, channel0,
           concurrency_deterministic_channel_event(3, 0), true,
           CONCURRENCY_DEPENDENCE_TASK_ORDER);
    expect(&trace, concurrency_deterministic_channel_event(4, 0),
           concurrency_deterministic_channel_event(5, 0), true,
           CONCURRENCY_DEPENDENCE_AUTHORITY);
    expect(&trace, concurrency_deterministic_task_event(0, 0), channel1,
           true, CONCURRENCY_DEPENDENCE_TASK_LIFETIME);
    expect(&trace, concurrency_deterministic_task_event(1, 0),
           concurrency_deterministic_channel_event(6, 0), true,
           CONCURRENCY_DEPENDENCE_CLEANUP);
    expect(&trace, concurrency_deterministic_task_event(2, 0),
           concurrency_deterministic_channel_event(4, 0), true,
           CONCURRENCY_DEPENDENCE_AUTHORITY);

    ConcurrencyDependence invalid = {0};
    assert(concurrency_deterministic_dependence(
               &trace, concurrency_deterministic_task_event(9, 0), channel0,
               &invalid) == CONCURRENCY_DETERMINISTIC_INVALID_PROJECTION);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "dependence_test.c"
            binary_path = Path(directory) / "dependence_test"
            source_path.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(source_path),
                    str(ROOT / "concurrency" / "dependence.c"),
                    str(ROOT / "concurrency" / "deterministic.c"),
                    str(ROOT / "concurrency" / "channel.c"),
                    str(ROOT / "concurrency" / "channel_machine.c"),
                    str(ROOT / "concurrency" / "channel_effect.c"),
                    str(ROOT / "concurrency" / "cleanup_bridge.c"),
                    str(ROOT / "concurrency" / "kernel.c"),
                    str(ROOT / "concurrency" / "effect.c"),
                    str(ROOT / "concurrency" / "network.c"),
                    str(ROOT / "concurrency" / "network_trace.c"),
                    str(ROOT / "effects" / "effect.c"),
                    str(ROOT / "qtt" / "place.c"),
                    str(ROOT / "qtt" / "quantity.c"),
                    "-o", str(binary_path),
                ],
                check=True,
            )
            environment = os.environ.copy()
            environment["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(binary_path)], env=environment, check=True)


if __name__ == "__main__":
    unittest.main()
