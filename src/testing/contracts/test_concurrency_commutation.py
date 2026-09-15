"""Cycle 5.7: independent co-enabled events commute executably."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


class ConcurrencyCommutationTests(unittest.TestCase):
    def test_adjacent_task_channel_swap_preserves_semantic_state(self):
        source = r'''
#include "concurrency/coverage.h"
#include "concurrency/process.h"
#include "concurrency/process_commutation.h"

#include <assert.h>

static ConcurrencyTaskId task(uint64_t id) {
    return (ConcurrencyTaskId){.module_id = 31, .task_id = id};
}

static ConcurrencySessionAction action(
    ConcurrencySessionActionKind kind, uint64_t payload) {
    return (ConcurrencySessionAction){
        .kind = kind, .payload_type_id = payload,
    };
}

static ConcurrencyNetworkInitialAuthority authority(
    uint64_t id, uint64_t protocol, ConcurrencyTaskId owner) {
    return (ConcurrencyNetworkInitialAuthority){
        .authority = {
            .endpoint = {.module_id = 31, .endpoint_id = id},
            .place = qtt_place_root((QttCoreVar){
                .module_id = 31, .binder_id = id,
            }),
            .quantity = qtt_quantity_finite(1),
            .protocol_type_id = protocol,
        },
        .owner = owner,
    };
}

int main(void) {
    ConcurrencySessionAction sender[] = {
        action(CONCURRENCY_SESSION_SEND, 500),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencySessionAction receiver[] = {
        action(CONCURRENCY_SESSION_RECEIVE, 500),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencyNetworkChannel channel = {
        .channel_id = {.module_id = 31, .channel_id = 1},
        .left = {
            .endpoint_id = 100, .owner = task(2),
            .quantity = qtt_quantity_finite(1),
            .protocol = sender, .protocol_count = 2,
        },
        .right = {
            .endpoint_id = 101, .owner = task(3),
            .quantity = qtt_quantity_finite(1),
            .protocol = receiver, .protocol_count = 2,
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
    for (int operation = CONCURRENCY_CHANNEL_EFFECT_CANCEL;
         operation >= CONCURRENCY_CHANNEL_EFFECT_SEND; operation--) {
        QttEffectAtom atom = concurrency_channel_effect_atom(
            (ConcurrencyChannelEffectOperation)operation, 77, 700);
        effects = qtt_effect_extend_atom(arena, &atom, effects);
        assert(effects);
    }
    QttEffectSolver *solver = qtt_effect_solver_new(arena);
    assert(solver);
    ConcurrencyNetworkTrace network = {
        .channels = &channel, .channel_count = 1,
        .authorities = authorities, .authority_count = 3,
        .steps = network_steps, .step_count = 2,
        .effect_solver = solver, .effects = effects,
    };
    ConcurrencyStep task_steps[] = {
        concurrency_spawn(task(1), task(2), NULL, 0),
        concurrency_spawn(task(1), task(3), NULL, 0),
        concurrency_join(task(1), task(3)),
        concurrency_cancel(task(1), task(2)),
        concurrency_checkpoint(task(2)),
        concurrency_cleanup_complete(task(2)),
        concurrency_join(task(1), task(2)),
    };
    ConcurrencyTrace tasks = {
        .root = task(1), .steps = task_steps, .step_count = 7,
    };
    ConcurrencyDeterministicEvent schedule[] = {
        concurrency_deterministic_task_event(0, 0),
        concurrency_deterministic_task_event(1, 1),
        concurrency_deterministic_channel_event(0, 2),
        concurrency_deterministic_task_event(2, 3),
        concurrency_deterministic_task_event(3, 4),
        concurrency_deterministic_task_event(4, 5),
        concurrency_deterministic_channel_event(1, 6),
        concurrency_deterministic_task_event(5, 7),
        concurrency_deterministic_task_event(6, 8),
    };
    ConcurrencyDeterministicTrace trace = {
        .task_trace = &tasks, .network_trace = &network,
        .events = schedule, .event_count = 9,
    };

    ConcurrencyProcessEvent root_events[] = {
        concurrency_process_event(CONCURRENCY_DETERMINISTIC_TASK_EVENT, 0),
        concurrency_process_event(CONCURRENCY_DETERMINISTIC_TASK_EVENT, 1),
        concurrency_process_event(CONCURRENCY_DETERMINISTIC_TASK_EVENT, 2),
        concurrency_process_event(CONCURRENCY_DETERMINISTIC_TASK_EVENT, 3),
        concurrency_process_event(CONCURRENCY_DETERMINISTIC_TASK_EVENT, 6),
    };
    ConcurrencyProcessEvent worker_events[] = {
        concurrency_process_event(CONCURRENCY_DETERMINISTIC_TASK_EVENT, 4),
        concurrency_process_event(CONCURRENCY_DETERMINISTIC_TASK_EVENT, 5),
    };
    ConcurrencyProcessEvent io_events[] = {
        concurrency_process_event(CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT, 0),
        concurrency_process_event(CONCURRENCY_DETERMINISTIC_CHANNEL_EVENT, 1),
    };
    ConcurrencyProcess streams[] = {
        {
            .id = {.module_id = 31, .process_id = 1},
            .events = root_events, .event_count = 5,
        },
        {
            .id = {.module_id = 31, .process_id = 2},
            .events = worker_events, .event_count = 2,
        },
        {
            .id = {.module_id = 31, .process_id = 3},
            .events = io_events, .event_count = 2,
        },
    };
    ConcurrencyProcessProgram program = {
        .processes = streams, .process_count = 3,
    };
    ConcurrencyProcessChoice choices[] = {
        concurrency_process_choice(streams[0].id, 0, 0),
        concurrency_process_choice(streams[0].id, 1, 1),
        concurrency_process_choice(streams[2].id, 0, 2),
        concurrency_process_choice(streams[0].id, 2, 3),
        concurrency_process_choice(streams[0].id, 3, 4),
        concurrency_process_choice(streams[1].id, 0, 5),
        concurrency_process_choice(streams[2].id, 1, 6),
        concurrency_process_choice(streams[1].id, 1, 7),
        concurrency_process_choice(streams[0].id, 4, 8),
    };
    ConcurrencyProcessSchedule process_schedule = {
        .choices = choices, .choice_count = 9,
    };
    ConcurrencyProcessCertificate process_certificate = {0};
    assert(concurrency_process_run(
               &trace, &program, &process_schedule,
               &process_certificate) == CONCURRENCY_PROCESS_CERTIFIED);
    assert(process_certificate.process_count == 3);
    assert(process_certificate.event_count == 9);
    assert(concurrency_process_verify(
               &trace, &program, &process_schedule,
               &process_certificate) == CONCURRENCY_PROCESS_CERTIFIED);
    ConcurrencyProcessSchedule process_prefix = {
        .choices = choices, .choice_count = 2,
    };
    ConcurrencyProcessFrontier frontier = {0};
    assert(concurrency_process_frontier_build(
               &trace, &program, &process_prefix, &frontier) ==
           CONCURRENCY_PROCESS_CERTIFIED);
    assert(frontier.process_count == 3);
    assert(frontier.enabled_count == 2);
    assert(frontier.entries[0].enabled);
    assert(frontier.entries[0].next_local_index == 2);
    assert(!frontier.entries[1].enabled);
    assert(frontier.entries[1].blocked ==
           CONCURRENCY_PROCESS_PREDECESSOR_PENDING);
    assert(frontier.entries[2].enabled);
    assert(frontier.binary.next_task_index == 2);
    assert(frontier.binary.next_channel_index == 0);
    assert(concurrency_process_frontier_verify(
               &trace, &program, &process_prefix, &frontier) ==
           CONCURRENCY_PROCESS_CERTIFIED);
    frontier.fingerprint++;
    assert(concurrency_process_frontier_verify(
               &trace, &program, &process_prefix, &frontier) ==
           CONCURRENCY_PROCESS_INVALID_CERTIFICATE);
    frontier.fingerprint--;
    concurrency_process_frontier_free(&frontier);
    process_prefix.choice_count = 1;
    ConcurrencyProcessCommutationCertificate process_diamond = {0};
    assert(concurrency_process_commute(
               &trace, &program, &process_prefix,
               streams[0].id, streams[2].id, &process_diamond) ==
           CONCURRENCY_PROCESS_COMMUTATION_CERTIFIED);
    assert(!process_diamond.dependence.dependent);
    assert(process_diamond.left_local_index == 1);
    assert(process_diamond.right_local_index == 0);
    assert(process_diamond.binary.forward_state_fingerprint ==
           process_diamond.binary.reverse_state_fingerprint);
    assert(concurrency_process_commutation_verify(
               &trace, &program, &process_prefix, &process_diamond) ==
           CONCURRENCY_PROCESS_COMMUTATION_CERTIFIED);
    process_diamond.left_local_index++;
    assert(concurrency_process_commutation_verify(
               &trace, &program, &process_prefix, &process_diamond) ==
           CONCURRENCY_PROCESS_COMMUTATION_INVALID_CERTIFICATE);
    process_diamond.left_local_index--;
    assert(concurrency_process_commute(
               &trace, &program, &process_prefix,
               streams[2].id, streams[0].id, &process_diamond) ==
           CONCURRENCY_PROCESS_COMMUTATION_CERTIFIED);
    assert(process_diamond.left_process.process_id == 3);
    assert(process_diamond.right_process.process_id == 1);
    assert(concurrency_process_commute(
               &trace, &program, &process_prefix,
               streams[0].id, streams[0].id, &process_diamond) ==
           CONCURRENCY_PROCESS_COMMUTATION_SAME_PROCESS);
    process_prefix.choice_count = 0;
    assert(concurrency_process_commute(
               &trace, &program, &process_prefix,
               streams[0].id, streams[2].id, &process_diamond) ==
           CONCURRENCY_PROCESS_COMMUTATION_NOT_COENABLED);
    for (size_t prefix_count = 0; prefix_count <= 9; prefix_count++) {
        process_prefix.choice_count = prefix_count;
        assert(concurrency_process_frontier_build(
                   &trace, &program, &process_prefix, &frontier) ==
               CONCURRENCY_PROCESS_CERTIFIED);
        size_t independently_enabled = 0;
        for (size_t i = 0; i < frontier.process_count; i++) {
            ConcurrencyProcessFrontierEntry entry = frontier.entries[i];
            if (entry.blocked == CONCURRENCY_PROCESS_FRONTIER_ENABLED) {
                assert(entry.enabled);
                independently_enabled++;
                if (entry.next_kind == CONCURRENCY_DETERMINISTIC_TASK_EVENT) {
                    assert(entry.next_projection_index ==
                           frontier.binary.next_task_index);
                    assert(frontier.binary.task_enabled);
                } else {
                    assert(entry.next_projection_index ==
                           frontier.binary.next_channel_index);
                    assert(frontier.binary.channel_enabled);
                }
            }
        }
        assert(independently_enabled == frontier.enabled_count);
        assert(concurrency_process_frontier_verify(
                   &trace, &program, &process_prefix, &frontier) ==
               CONCURRENCY_PROCESS_CERTIFIED);
        concurrency_process_frontier_free(&frontier);
    }
    process_prefix.choice_count = 2;
    choices[2].local_index = 1;
    assert(concurrency_process_verify(
               &trace, &program, &process_schedule,
               &process_certificate) ==
           CONCURRENCY_PROCESS_INVALID_CERTIFICATE);
    choices[2].local_index = 0;
    process_certificate.fingerprint++;
    assert(concurrency_process_verify(
               &trace, &program, &process_schedule,
               &process_certificate) ==
           CONCURRENCY_PROCESS_INVALID_CERTIFICATE);
    process_certificate.fingerprint--;
    ConcurrencyProcessId saved_process = streams[2].id;
    streams[2].id = streams[1].id;
    assert(concurrency_process_run(
               &trace, &program, &process_schedule,
               &process_certificate) == CONCURRENCY_PROCESS_INVALID_PROGRAM);
    streams[2].id = saved_process;
    streams[2].event_count = 1;
    assert(concurrency_process_run(
               &trace, &program, &process_schedule,
               &process_certificate) == CONCURRENCY_PROCESS_INVALID_PROGRAM);
    streams[2].event_count = 2;

    ConcurrencyCommutationCertificate certificate = {0};
    assert(concurrency_deterministic_commute(&trace, 1, &certificate) ==
           CONCURRENCY_COMMUTATION_CERTIFIED);
    assert(certificate.prefix_count == 1);
    assert(certificate.forward_state_fingerprint ==
           certificate.reverse_state_fingerprint);
    assert(concurrency_deterministic_commutation_verify(
               &trace, &certificate) == CONCURRENCY_COMMUTATION_CERTIFIED);

    certificate.reverse_state_fingerprint++;
    assert(concurrency_deterministic_commutation_verify(
               &trace, &certificate) ==
           CONCURRENCY_COMMUTATION_INVALID_CERTIFICATE);

    ConcurrencyStep cold_steps[] = {
        concurrency_spawn(task(1), task(3), NULL, 0),
        concurrency_join(task(1), task(3)),
        concurrency_spawn(task(1), task(2), NULL, 0),
        concurrency_cancel(task(1), task(2)),
        concurrency_checkpoint(task(2)),
        concurrency_cleanup_complete(task(2)),
        concurrency_join(task(1), task(2)),
    };
    ConcurrencyTrace cold_tasks = {
        .root = task(1), .steps = cold_steps, .step_count = 7,
    };
    ConcurrencyDeterministicEvent cold_schedule[] = {
        concurrency_deterministic_task_event(0, 0),
        concurrency_deterministic_task_event(1, 1),
        concurrency_deterministic_task_event(2, 2),
        concurrency_deterministic_task_event(3, 3),
        concurrency_deterministic_task_event(4, 4),
        concurrency_deterministic_channel_event(0, 5),
        concurrency_deterministic_channel_event(1, 6),
        concurrency_deterministic_task_event(5, 7),
        concurrency_deterministic_task_event(6, 8),
    };
    ConcurrencyDeterministicTrace cold = {
        .task_trace = &cold_tasks, .network_trace = &network,
        .events = cold_schedule, .event_count = 9,
    };
    assert(concurrency_deterministic_commute(&cold, 0, &certificate) ==
           CONCURRENCY_COMMUTATION_NOT_COENABLED);

    assert(concurrency_deterministic_commute(&trace, 0, &certificate) ==
           CONCURRENCY_COMMUTATION_DEPENDENT);

    ConcurrencyExploration exhaustive = {0};
    ConcurrencyExploration reduced = {0};
    ConcurrencyExplorationLimits limits = {
        .max_schedules = 128,
    };
    assert(concurrency_deterministic_explore(
               &trace, CONCURRENCY_EXPLORATION_EXHAUSTIVE,
               limits, &exhaustive) == CONCURRENCY_EXPLORATION_CERTIFIED);
    assert(concurrency_deterministic_explore(
               &trace, CONCURRENCY_EXPLORATION_CERTIFIED_SOURCE,
               limits, &reduced) == CONCURRENCY_EXPLORATION_CERTIFIED);
    assert(exhaustive.schedule_count > reduced.schedule_count);
    assert(reduced.schedule_count > 0);
    assert(reduced.commutation_count > 0);
    assert(reduced.pruned_choice_count > 0);
    assert(reduced.fallback_branch_count > 0);
    for (size_t i = 0; i < reduced.schedule_count; i++) {
        ConcurrencyDeterministicTrace replay = trace;
        replay.events = reduced.schedules[i].events;
        replay.event_count = reduced.schedules[i].event_count;
        assert(concurrency_deterministic_verify(
                   &replay, &reduced.schedules[i].certificate) ==
               CONCURRENCY_DETERMINISTIC_CERTIFIED);
        for (size_t j = 0; j < i; j++)
            assert(reduced.schedules[i].certificate.schedule_fingerprint !=
                   reduced.schedules[j].certificate.schedule_fingerprint);
    }
    assert(concurrency_deterministic_exploration_verify(
               &trace, &reduced) == CONCURRENCY_EXPLORATION_CERTIFIED);
    reduced.schedules[0].certificate.final_tick++;
    assert(concurrency_deterministic_exploration_verify(
               &trace, &reduced) ==
           CONCURRENCY_EXPLORATION_INVALID_CERTIFICATE);
    reduced.schedules[0].certificate.final_tick--;
    reduced.fingerprint++;
    assert(concurrency_deterministic_exploration_verify(
               &trace, &reduced) ==
           CONCURRENCY_EXPLORATION_INVALID_CERTIFICATE);
    reduced.fingerprint--;

    ConcurrencyCoverage coverage = {0};
    ConcurrencyCoverageLimits coverage_limits = {
        .max_swaps_per_witness = 64,
    };
    assert(concurrency_deterministic_coverage_build(
               &trace, &exhaustive, &reduced,
               coverage_limits, &coverage) ==
           CONCURRENCY_COVERAGE_CERTIFIED);
    assert(coverage.witness_count == exhaustive.schedule_count);
    assert(coverage.covered_schedule_count == exhaustive.schedule_count);
    bool observed_swap = false;
    for (size_t i = 0; i < coverage.witness_count; i++)
        if (coverage.witnesses[i].swap_count) observed_swap = true;
    assert(observed_swap);
    assert(concurrency_deterministic_coverage_verify(
               &trace, &exhaustive, &reduced, &coverage) ==
           CONCURRENCY_COVERAGE_CERTIFIED);
    size_t saved_swap = 0;
    size_t mutated_witness = coverage.witness_count;
    for (size_t i = 0; i < coverage.witness_count; i++)
        if (coverage.witnesses[i].swap_count) {
            mutated_witness = i;
            saved_swap = coverage.witnesses[i].swap_positions[0];
            coverage.witnesses[i].swap_positions[0] =
                exhaustive.schedules[i].event_count;
            break;
        }
    assert(mutated_witness < coverage.witness_count);
    assert(concurrency_deterministic_coverage_verify(
               &trace, &exhaustive, &reduced, &coverage) ==
           CONCURRENCY_COVERAGE_INVALID_CERTIFICATE);
    coverage.witnesses[mutated_witness].swap_positions[0] = saved_swap;
    concurrency_deterministic_coverage_free(&coverage);
    concurrency_deterministic_exploration_free(&reduced);
    concurrency_deterministic_exploration_free(&exhaustive);
    limits.max_schedules = 1;
    assert(concurrency_deterministic_explore(
               &trace, CONCURRENCY_EXPLORATION_EXHAUSTIVE,
               limits, &exhaustive) ==
           CONCURRENCY_EXPLORATION_LIMIT_EXCEEDED);
    assert(exhaustive.schedules == NULL);

    qtt_effect_solver_free(solver);
    qtt_effect_arena_free(arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "commutation_test.c"
            binary_path = Path(directory) / "commutation_test"
            source_path.write_text(source)
            sources = [
                "process_commutation.c", "process.c", "coverage.c",
                "exploration.c", "commutation.c",
                "dependence.c",
                "deterministic.c",
                "channel.c", "channel_machine.c", "channel_effect.c",
                "cleanup_bridge.c", "kernel.c", "effect.c", "network.c",
                "network_trace.c",
            ]
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT / "src"), str(source_path),
                    *[str(ROOT / "src" / "concurrency" / name) for name in sources],
                    str(ROOT / "src" / "effects" / "effect.c"),
                    str(ROOT / "src" / "qtt" / "place.c"),
                    str(ROOT / "src" / "qtt" / "quantity.c"),
                    "-o", str(binary_path),
                ], check=True,
            )
            environment = os.environ.copy()
            environment["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(binary_path)], env=environment, check=True)


if __name__ == "__main__":
    unittest.main()
