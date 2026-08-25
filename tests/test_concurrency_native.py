"""Native N-process frontier and exploration milestone."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class NativeConcurrencyTests(unittest.TestCase):
    def test_three_processes_are_simultaneously_enabled(self):
        source = r'''
#include "concurrency/native.h"

#include <assert.h>

static ConcurrencyProcessId process(uint64_t id) {
    return (ConcurrencyProcessId){.module_id = 41, .process_id = id};
}

int main(void) {
    ConcurrencyNativeOperation operations[] = {
        concurrency_native_operation(101),
        concurrency_native_operation(102),
        concurrency_native_operation(103),
    };
    ConcurrencyNativeProcess processes[] = {
        {.id = process(1), .operations = &operations[0], .operation_count = 1},
        {.id = process(2), .operations = &operations[1], .operation_count = 1},
        {.id = process(3), .operations = &operations[2], .operation_count = 1},
    };
    ConcurrencyNativeProgram program = {
        .processes = processes, .process_count = 3,
    };
    ConcurrencyNativeFrontier frontier = {0};
    ConcurrencyNativeExploration exploration = {0};
    assert(concurrency_native_frontier(&program, NULL, 0, &frontier) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(frontier.enabled_count == 3);
    for (size_t i = 0; i < 3; i++) assert(frontier.entries[i].enabled);
    assert(concurrency_native_frontier_verify(
               &program, NULL, 0, &frontier) == CONCURRENCY_NATIVE_CERTIFIED);
    concurrency_native_frontier_free(&frontier);

    ConcurrencyNativeCommutationCertificate native_diamond = {0};
    assert(concurrency_native_commute(
               &program, process(1), process(2), NULL, 0, 64,
               &native_diamond) == CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED);
    assert(native_diamond.left_operation_id == 101);
    assert(native_diamond.right_operation_id == 102);
    assert(native_diamond.forward_state_fingerprint ==
           native_diamond.reverse_state_fingerprint);
    assert(native_diamond.forward_state_count ==
           native_diamond.reverse_state_count);
    assert(concurrency_native_commutation_verify(
               &program, NULL, 0, &native_diamond) ==
           CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED);
    native_diamond.forward_state_fingerprint++;
    assert(concurrency_native_commutation_verify(
               &program, NULL, 0, &native_diamond) ==
           CONCURRENCY_NATIVE_COMMUTATION_INVALID_CERTIFICATE);
    native_diamond.forward_state_fingerprint--;
    assert(concurrency_native_commute(
               &program, process(1), process(1), NULL, 0, 64,
               &native_diamond) ==
           CONCURRENCY_NATIVE_COMMUTATION_DEPENDENT);
    assert(concurrency_native_commute(
               &program, process(1), process(2), NULL, 0, 1,
               &native_diamond) ==
           CONCURRENCY_NATIVE_COMMUTATION_LIMIT_EXCEEDED);

    ConcurrencyStep task_steps[] = {
        concurrency_spawn((ConcurrencyTaskId){41, 1},
                          (ConcurrencyTaskId){41, 2}, NULL, 0),
        concurrency_join((ConcurrencyTaskId){41, 1},
                         (ConcurrencyTaskId){41, 2}),
        concurrency_spawn((ConcurrencyTaskId){41, 1},
                          (ConcurrencyTaskId){41, 3}, NULL, 0),
        concurrency_join((ConcurrencyTaskId){41, 1},
                         (ConcurrencyTaskId){41, 3}),
    };
    ConcurrencyTrace task_model = {
        .root = (ConcurrencyTaskId){41, 1},
        .steps = task_steps, .step_count = 4,
    };
    ConcurrencyNativeOperation task_operations[] = {
        concurrency_native_task_operation(201, 0),
        concurrency_native_task_operation(202, 1),
        concurrency_native_task_operation(203, 2),
        concurrency_native_task_operation(204, 3),
        concurrency_native_operation(205),
    };
    ConcurrencyNativeProcess task_processes[] = {
        {
            .id = process(11), .operations = &task_operations[0],
            .operation_count = 2,
        },
        {
            .id = process(12), .operations = &task_operations[2],
            .operation_count = 2,
        },
        {
            .id = process(13), .operations = &task_operations[4],
            .operation_count = 1,
        },
    };
    ConcurrencyNativeProgram task_program = {
        .processes = task_processes, .process_count = 3,
        .task_model = &task_model,
    };
    assert(concurrency_native_frontier(&task_program, NULL, 0, &frontier) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(frontier.enabled_count == 3);
    concurrency_native_frontier_free(&frontier);
    assert(concurrency_native_commute(
               &task_program, process(11), process(13), NULL, 0, 64,
               &native_diamond) == CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED);
    assert(native_diamond.forward_state_fingerprint ==
           native_diamond.reverse_state_fingerprint);
    assert(concurrency_native_commutation_verify(
               &task_program, NULL, 0, &native_diamond) ==
           CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED);
    assert(concurrency_native_explore(
               &task_program,
               (ConcurrencyNativeLimits){.max_schedules = 64},
               &exploration) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(exploration.schedule_count == 30);
    assert(concurrency_native_exploration_verify(
               &task_program, &exploration) == CONCURRENCY_NATIVE_CERTIFIED);
    concurrency_native_exploration_free(&exploration);
    ConcurrencyNativeOperation invalid_order[] = {
        concurrency_native_task_operation(202, 1),
        concurrency_native_task_operation(201, 0),
    };
    task_processes[0].operations = invalid_order;
    assert(concurrency_native_frontier(&task_program, NULL, 0, &frontier) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(!frontier.entries[0].enabled);
    assert(frontier.entries[1].enabled);
    assert(frontier.entries[2].enabled);
    assert(frontier.enabled_count == 2);
    concurrency_native_frontier_free(&frontier);
    task_processes[0].operations = &task_operations[0];

    ConcurrencyStep cancel_steps[] = {
        concurrency_spawn((ConcurrencyTaskId){41, 1},
                          (ConcurrencyTaskId){41, 4}, NULL, 0),
        concurrency_cancel((ConcurrencyTaskId){41, 1},
                           (ConcurrencyTaskId){41, 4}),
        concurrency_checkpoint((ConcurrencyTaskId){41, 4}),
        concurrency_cleanup_complete((ConcurrencyTaskId){41, 4}),
        concurrency_join((ConcurrencyTaskId){41, 1},
                         (ConcurrencyTaskId){41, 4}),
    };
    ConcurrencyTrace cancel_model = {
        .root = (ConcurrencyTaskId){41, 1},
        .steps = cancel_steps, .step_count = 5,
    };
    ConcurrencyNativeOperation cancel_root[] = {
        concurrency_native_task_operation(301, 0),
        concurrency_native_task_operation(302, 1),
        concurrency_native_task_operation(305, 4),
    };
    ConcurrencyNativeOperation cancel_child[] = {
        concurrency_native_task_operation(303, 2),
        concurrency_native_task_operation(304, 3),
    };
    ConcurrencyNativeOperation cancel_other[] = {
        concurrency_native_operation(306),
    };
    ConcurrencyNativeProcess cancel_processes[] = {
        {.id = process(21), .operations = cancel_root, .operation_count = 3},
        {.id = process(22), .operations = cancel_child, .operation_count = 2},
        {.id = process(23), .operations = cancel_other, .operation_count = 1},
    };
    ConcurrencyNativeProgram cancel_program = {
        .processes = cancel_processes, .process_count = 3,
        .task_model = &cancel_model,
    };
    assert(concurrency_native_explore(
               &cancel_program,
               (ConcurrencyNativeLimits){.max_schedules = 16},
               &exploration) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(exploration.schedule_count == 6);
    assert(exploration.dead_end_count > 0);
    assert(concurrency_native_exploration_verify(
               &cancel_program, &exploration) == CONCURRENCY_NATIVE_CERTIFIED);
    concurrency_native_exploration_free(&exploration);
    ConcurrencyNativeChoice after_spawn[] = {
        {.process = process(21), .local_index = 0, .tick = 0},
    };
    ConcurrencyNativeDependence task_dependence = {0};
    assert(concurrency_native_dependence(
               &cancel_program, process(21), process(22), after_spawn, 1,
               &task_dependence) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(task_dependence.dependent);
    assert(task_dependence.reasons &
           CONCURRENCY_NATIVE_DEPENDENCE_TASK_LIFETIME);
    assert(concurrency_native_commute(
               &cancel_program, process(21), process(22), after_spawn, 1, 64,
               &native_diamond) ==
           CONCURRENCY_NATIVE_COMMUTATION_DEPENDENT);

    assert(concurrency_native_explore(
               &program, (ConcurrencyNativeLimits){.max_schedules = 8},
               &exploration) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(exploration.schedule_count == 6);
    assert(exploration.branch_count == 4);
    assert(concurrency_native_exploration_verify(&program, &exploration) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    exploration.fingerprint++;
    assert(concurrency_native_exploration_verify(&program, &exploration) ==
           CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    exploration.fingerprint--;
    concurrency_native_exploration_free(&exploration);

    ConcurrencyNativeExploration reduced = {0};
    ConcurrencyNativeLimits reduced_limits = {
        .max_schedules = 8,
        .max_commutation_states = 64,
    };
    assert(concurrency_native_explore_reduced(
               &program, reduced_limits, &reduced) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(reduced.schedule_count == 1);
    assert(reduced.commutation_count == 3);
    assert(reduced.pruned_choice_count == 3);
    assert(concurrency_native_exploration_verify(&program, &reduced) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    ConcurrencyNativeExploration exhaustive = {0};
    assert(concurrency_native_explore(
               &program, reduced_limits, &exhaustive) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    ConcurrencyNativeCoverage coverage = {0};
    assert(concurrency_native_coverage_build(
               &program, &exhaustive, &reduced,
               (ConcurrencyNativeCoverageLimits){
                   .max_swaps_per_witness = 3,
                   .max_commutation_states = 64,
               },
               &coverage) == CONCURRENCY_NATIVE_COVERAGE_CERTIFIED);
    assert(coverage.witness_count == 6);
    assert(coverage.covered_schedule_count == 6);
    assert(coverage.total_swap_count == 9);
    assert(concurrency_native_coverage_verify(
               &program, &exhaustive, &reduced, &coverage) ==
           CONCURRENCY_NATIVE_COVERAGE_CERTIFIED);
    coverage.total_swap_count++;
    assert(concurrency_native_coverage_verify(
               &program, &exhaustive, &reduced, &coverage) ==
           CONCURRENCY_NATIVE_COVERAGE_INVALID_CERTIFICATE);
    coverage.total_swap_count--;
    concurrency_native_coverage_free(&coverage);
    concurrency_native_exploration_free(&exhaustive);
    concurrency_native_exploration_free(&reduced);

    QttPlace shared = qtt_place_root((QttCoreVar){
        .module_id = 41, .binder_id = 900,
    });
    operations[0].writes = &shared;
    operations[0].write_count = 1;
    operations[1].reads = &shared;
    operations[1].read_count = 1;
    ConcurrencyNativeDependence dependence = {0};
    assert(concurrency_native_dependence(
               &program, process(1), process(2), NULL, 0, &dependence) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(dependence.dependent);
    assert(dependence.reasons & CONCURRENCY_NATIVE_DEPENDENCE_QTT_PLACE);

    ConcurrencyProcessId sleepers[] = {process(1), process(3)};
    ConcurrencyNativeSleepStepCertificate sleep_step = {0};
    assert(concurrency_native_sleep_step(
               &program, NULL, 0, sleepers, 2, process(2), &sleep_step) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(sleep_step.input_count == 2);
    assert(sleep_step.retained_count == 1);
    assert(sleep_step.retained[0].process_id == 3);
    assert(!sleep_step.decisions[0].retained);
    assert(sleep_step.decisions[0].reasons &
           CONCURRENCY_NATIVE_DEPENDENCE_QTT_PLACE);
    assert(sleep_step.decisions[1].retained);
    assert(sleep_step.decisions[1].reasons == 0);
    assert(concurrency_native_sleep_step_verify(
               &program, NULL, 0, sleepers, 2, process(2), &sleep_step) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    sleep_step.decisions[1].retained = false;
    assert(concurrency_native_sleep_step_verify(
               &program, NULL, 0, sleepers, 2, process(2), &sleep_step) ==
           CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    sleep_step.decisions[1].retained = true;
    concurrency_native_sleep_step_free(&sleep_step);

    ConcurrencyNativeChoice race_schedule[] = {
        {.process = process(1), .local_index = 0, .tick = 0},
        {.process = process(3), .local_index = 0, .tick = 1},
        {.process = process(2), .local_index = 0, .tick = 2},
    };
    ConcurrencyNativeRaceCertificate races = {0};
    assert(concurrency_native_races(
               &program, race_schedule, 3, &races) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(races.occurrence_count == 3);
    assert(races.happens_before_edge_count == 1);
    assert(concurrency_native_happens_before(&races, 0, 2));
    assert(!concurrency_native_happens_before(&races, 0, 1));
    assert(races.race_count == 1);
    assert(races.races[0].earlier_index == 0);
    assert(races.races[0].later_index == 2);
    assert(races.races[0].reversal_prefix_count == 0);
    assert(races.races[0].reversal_count == 2);
    assert(races.races[0].reversal[0].process.process_id == 3);
    assert(races.races[0].reversal[1].process.process_id == 2);
    assert(races.races[0].reasons &
           CONCURRENCY_NATIVE_DEPENDENCE_QTT_PLACE);
    assert(concurrency_native_races_verify(
               &program, race_schedule, 3, &races) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    races.races[0].later_index = 1;
    assert(concurrency_native_races_verify(
               &program, race_schedule, 3, &races) ==
           CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    races.races[0].later_index = 2;
    concurrency_native_races_free(&races);

    ConcurrencyNativeChoice weak_left[] = {
        {.process = process(3), .local_index = 0, .tick = 0},
    };
    ConcurrencyNativeChoice weak_right[] = {
        {.process = process(2), .local_index = 0, .tick = 0},
    };
    ConcurrencyNativeWeakInitialCertificate weak = {0};
    assert(concurrency_native_weak_initial(
               &program, NULL, 0, weak_left, 1, weak_right, 1,
               (ConcurrencyNativeLimits){
                   .max_schedules = 8,
                   .max_commutation_states = 64,
               },
               &weak) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(weak.related);
    assert(concurrency_native_weak_initial_verify(
               &program, NULL, 0, weak_left, 1, weak_right, 1, &weak) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    weak.related = false;
    assert(concurrency_native_weak_initial_verify(
               &program, NULL, 0, weak_left, 1, weak_right, 1, &weak) ==
           CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    weak.related = true;
    concurrency_native_weak_initial_free(&weak);
    assert(concurrency_native_weak_initial(
               &program, NULL, 0, race_schedule, 1, weak_right, 1,
               (ConcurrencyNativeLimits){
                   .max_schedules = 8,
                   .max_commutation_states = 64,
               },
               &weak) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(!weak.related);
    concurrency_native_weak_initial_free(&weak);

    ConcurrencyNativeChoice initial_right[] = {
        {.process = process(2), .local_index = 0, .tick = 0},
        {.process = process(3), .local_index = 0, .tick = 1},
    };
    ConcurrencyNativeInitialCertificate initial = {0};
    assert(concurrency_native_initial(
               &program, NULL, 0, weak_left, 1, initial_right, 2,
               (ConcurrencyNativeLimits){
                   .max_commutation_states = 64,
                   .max_equivalence_states = 8,
               },
               &initial) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(initial.related);
    assert(initial.swap_count == 1);
    assert(initial.swap_positions[0] == 0);
    assert(concurrency_native_initial_verify(
               &program, NULL, 0, weak_left, 1, initial_right, 2,
               &initial) == CONCURRENCY_NATIVE_CERTIFIED);
    initial.swap_positions[0] = 1;
    assert(concurrency_native_initial_verify(
               &program, NULL, 0, weak_left, 1, initial_right, 2,
               &initial) == CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    initial.swap_positions[0] = 0;
    concurrency_native_initial_free(&initial);

    ConcurrencyNativeChoice dependent_right[] = {
        {.process = process(2), .local_index = 0, .tick = 0},
        {.process = process(1), .local_index = 0, .tick = 1},
    };
    assert(concurrency_native_initial(
               &program, NULL, 0, race_schedule, 1, dependent_right, 2,
               (ConcurrencyNativeLimits){
                   .max_commutation_states = 64,
                   .max_equivalence_states = 8,
               },
               &initial) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(!initial.related);
    assert(concurrency_native_initial_verify(
               &program, NULL, 0, race_schedule, 1, dependent_right, 2,
               &initial) == CONCURRENCY_NATIVE_CERTIFIED);
    concurrency_native_initial_free(&initial);

    ConcurrencyNativeWakeupAdmissibilityCertificate admissibility = {0};
    assert(concurrency_native_wakeup_admissible(
               &program, NULL, 0, weak_right, 1, sleepers, 1,
               (ConcurrencyNativeLimits){
                   .max_schedules = 8,
                   .max_commutation_states = 64,
               },
               &admissibility) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(admissibility.admissible);
    assert(admissibility.decision_count == 1);
    assert(!admissibility.decisions[0].weak_initial.related);
    assert(concurrency_native_wakeup_admissible_verify(
               &program, NULL, 0, weak_right, 1, sleepers, 1,
               &admissibility) == CONCURRENCY_NATIVE_CERTIFIED);
    admissibility.decisions[0].weak_initial.related = true;
    assert(concurrency_native_wakeup_admissible_verify(
               &program, NULL, 0, weak_right, 1, sleepers, 1,
               &admissibility) == CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    admissibility.decisions[0].weak_initial.related = false;
    admissibility.admissible = false;
    assert(concurrency_native_wakeup_admissible_verify(
               &program, NULL, 0, weak_right, 1, sleepers, 1,
               &admissibility) == CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    admissibility.admissible = true;
    concurrency_native_wakeup_admissibility_free(&admissibility);

    assert(concurrency_native_wakeup_admissible(
               &program, NULL, 0, weak_right, 1, &sleepers[1], 1,
               (ConcurrencyNativeLimits){
                   .max_schedules = 8,
                   .max_commutation_states = 64,
               },
               &admissibility) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(!admissibility.admissible);
    assert(admissibility.blocking_sleep_index == 0);
    assert(admissibility.decisions[0].weak_initial.related);
    assert(concurrency_native_wakeup_admissible_verify(
               &program, NULL, 0, weak_right, 1, &sleepers[1], 1,
               &admissibility) == CONCURRENCY_NATIVE_CERTIFIED);
    concurrency_native_wakeup_admissibility_free(&admissibility);
    assert(concurrency_native_wakeup_admissible(
               &program, NULL, 0, weak_right, 1, NULL, 0,
               (ConcurrencyNativeLimits){
                   .max_schedules = 8,
                   .max_commutation_states = 64,
               },
               &admissibility) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(admissibility.admissible);
    assert(admissibility.decision_count == 0);
    assert(concurrency_native_wakeup_admissible_verify(
               &program, NULL, 0, weak_right, 1, NULL, 0,
               &admissibility) == CONCURRENCY_NATIVE_CERTIFIED);
    concurrency_native_wakeup_admissibility_free(&admissibility);

    ConcurrencyNativeSchedule writer_leaf = {
        .choices = race_schedule,
        .choice_count = 1,
    };
    ConcurrencyNativeWakeupInsertCertificate insertion = {0};
    assert(concurrency_native_wakeup_insert(
               &program, NULL, 0, NULL, 0, &writer_leaf, 1,
               weak_right, 1,
               (ConcurrencyNativeLimits){
                   .max_schedules = 8,
                   .max_commutation_states = 64,
                   .max_equivalence_states = 16,
               },
               &insertion) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(!insertion.suppressed);
    assert(insertion.before_node_count == 2);
    assert(insertion.node_count == 3);
    assert(insertion.nodes[1].terminal);
    assert(insertion.nodes[2].terminal);
    assert(insertion.nodes[2].choice.process.process_id == 2);
    assert(concurrency_native_wakeup_insert_verify(
               &program, NULL, 0, NULL, 0, &writer_leaf, 1,
               weak_right, 1, &insertion) == CONCURRENCY_NATIVE_CERTIFIED);
    insertion.nodes[2].terminal = false;
    assert(concurrency_native_wakeup_insert_verify(
               &program, NULL, 0, NULL, 0, &writer_leaf, 1,
               weak_right, 1, &insertion) ==
           CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    insertion.nodes[2].terminal = true;
    concurrency_native_wakeup_insert_free(&insertion);

    ConcurrencyNativeSchedule unrelated_leaf = {
        .choices = weak_left,
        .choice_count = 1,
    };
    assert(concurrency_native_wakeup_insert(
               &program, NULL, 0, NULL, 0, &unrelated_leaf, 1,
               weak_right, 1,
               (ConcurrencyNativeLimits){
                   .max_schedules = 8,
                   .max_commutation_states = 64,
                   .max_equivalence_states = 16,
               },
               &insertion) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(insertion.suppressed);
    assert(insertion.anchor_node_index == 1);
    assert(insertion.before_node_count == insertion.node_count);
    assert(concurrency_native_wakeup_insert_verify(
               &program, NULL, 0, NULL, 0, &unrelated_leaf, 1,
               weak_right, 1, &insertion) == CONCURRENCY_NATIVE_CERTIFIED);
    concurrency_native_wakeup_insert_free(&insertion);

    assert(concurrency_native_explore_reduced(
               &program, reduced_limits, &reduced) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(reduced.schedule_count == 4);
    ConcurrencyNativeExploration wakeup = {0};
    assert(concurrency_native_explore_wakeup_tree(
               &program, reduced_limits, &wakeup) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(wakeup.schedule_count == 2);
    assert(wakeup.equivalence_class_count == 2);
    assert(wakeup.wakeup_node_count == 7);
    assert(concurrency_native_exploration_verify(&program, &wakeup) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    wakeup.equivalence_class_count++;
    assert(concurrency_native_exploration_verify(&program, &wakeup) ==
           CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    wakeup.equivalence_class_count--;
    wakeup.wakeup_node_count++;
    assert(concurrency_native_exploration_verify(&program, &wakeup) ==
           CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    wakeup.wakeup_node_count--;
    ConcurrencyNativeExploration bounded_wakeup = {0};
    assert(concurrency_native_explore_wakeup_tree(
               &program,
               (ConcurrencyNativeLimits){
                   .max_schedules = 8,
                   .max_commutation_states = 1,
               },
               &bounded_wakeup) == CONCURRENCY_NATIVE_LIMIT_EXCEEDED);
    assert(bounded_wakeup.schedules == NULL);
    assert(concurrency_native_explore(
               &program, reduced_limits, &exhaustive) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(concurrency_native_coverage_build(
               &program, &exhaustive, &wakeup,
               (ConcurrencyNativeCoverageLimits){
                   .max_swaps_per_witness = 2,
                   .max_commutation_states = 64,
               },
               &coverage) == CONCURRENCY_NATIVE_COVERAGE_CERTIFIED);
    assert(coverage.covered_schedule_count == 6);
    assert(concurrency_native_coverage_verify(
               &program, &exhaustive, &wakeup, &coverage) ==
           CONCURRENCY_NATIVE_COVERAGE_CERTIFIED);
    concurrency_native_coverage_free(&coverage);
    concurrency_native_exploration_free(&exhaustive);
    concurrency_native_exploration_free(&wakeup);

    ConcurrencyNativeLimits online_limits = {
        .max_schedules = 2,
        .max_commutation_states = 64,
        .max_equivalence_states = 8,
    };
    assert(concurrency_native_explore_wakeup_tree(
               &program, online_limits, &wakeup) ==
           CONCURRENCY_NATIVE_LIMIT_EXCEEDED);
    assert(concurrency_native_explore_wakeup_tree_online(
               &program, online_limits, &wakeup) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(wakeup.mode ==
           CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ONLINE_WAKEUP_TREE);
    assert(wakeup.schedule_count == 2);
    assert(wakeup.equivalence_class_count == 2);
    assert(wakeup.examined_schedule_count == 6);
    assert(wakeup.redundant_schedule_count == 4);
    assert(wakeup.wakeup_node_count == 7);
    assert(concurrency_native_exploration_verify(&program, &wakeup) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    wakeup.examined_schedule_count++;
    assert(concurrency_native_exploration_verify(&program, &wakeup) ==
           CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    wakeup.examined_schedule_count--;
    assert(concurrency_native_explore_wakeup_tree_online(
               &program,
               (ConcurrencyNativeLimits){
                   .max_schedules = 2,
                   .max_commutation_states = 64,
                   .max_equivalence_states = 1,
               },
               &bounded_wakeup) == CONCURRENCY_NATIVE_LIMIT_EXCEEDED);
    assert(concurrency_native_explore(
               &program,
               (ConcurrencyNativeLimits){.max_schedules = 8},
               &exhaustive) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(concurrency_native_coverage_build(
               &program, &exhaustive, &wakeup,
               (ConcurrencyNativeCoverageLimits){
                   .max_swaps_per_witness = 2,
                   .max_commutation_states = 64,
               },
               &coverage) == CONCURRENCY_NATIVE_COVERAGE_CERTIFIED);
    assert(coverage.covered_schedule_count == 6);
    concurrency_native_coverage_free(&coverage);
    concurrency_native_exploration_free(&exhaustive);
    concurrency_native_exploration_free(&wakeup);

    ConcurrencyNativeExploration race_wakeup = {0};
    assert(concurrency_native_explore_race_wakeup(
               &program,
               (ConcurrencyNativeLimits){
                   .max_schedules = 2,
                   .max_commutation_states = 64,
                   .max_equivalence_states = 8,
                   .max_wakeup_obligations = 8,
               },
               &race_wakeup) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(race_wakeup.mode ==
           CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_RACE_WAKEUP);
    assert(race_wakeup.schedule_count == 2);
    assert(race_wakeup.examined_schedule_count == 2);
    assert(race_wakeup.redundant_schedule_count == 0);
    assert(race_wakeup.wakeup_obligation_count == 1);
    assert(race_wakeup.discharged_wakeup_obligation_count == 1);
    assert(race_wakeup.wakeup_node_count == 7);
    assert(concurrency_native_exploration_verify(&program, &race_wakeup) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    race_wakeup.wakeup_obligations[0].discharged = false;
    assert(concurrency_native_exploration_verify(&program, &race_wakeup) ==
           CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    race_wakeup.wakeup_obligations[0].discharged = true;
    assert(concurrency_native_explore(
               &program,
               (ConcurrencyNativeLimits){.max_schedules = 8},
               &exhaustive) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(concurrency_native_coverage_build(
               &program, &exhaustive, &race_wakeup,
               (ConcurrencyNativeCoverageLimits){
                   .max_swaps_per_witness = 2,
                   .max_commutation_states = 64,
               },
               &coverage) == CONCURRENCY_NATIVE_COVERAGE_CERTIFIED);
    assert(coverage.covered_schedule_count == 6);
    concurrency_native_coverage_free(&coverage);
    concurrency_native_exploration_free(&exhaustive);
    concurrency_native_exploration_free(&race_wakeup);
    ConcurrencyNativeExploration ordered_wakeup = {0};
    assert(concurrency_native_explore_ordered_wakeup(
               &program,
               (ConcurrencyNativeLimits){
                   .max_schedules = 2,
                   .max_commutation_states = 64,
                   .max_equivalence_states = 16,
                   .max_wakeup_obligations = 8,
               },
               &ordered_wakeup) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(ordered_wakeup.mode ==
           CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ORDERED_WAKEUP);
    assert(ordered_wakeup.schedule_count == 2);
    assert(ordered_wakeup.ordered_insertion_count == 1);
    assert(ordered_wakeup.suppressed_insertion_count == 0);
    assert(ordered_wakeup.sleep_transition_count == 1);
    assert(ordered_wakeup.sleep_blocked_count == 0);
    assert(concurrency_native_exploration_verify(&program, &ordered_wakeup) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    ordered_wakeup.sleep_blocked_count++;
    assert(concurrency_native_exploration_verify(&program, &ordered_wakeup) ==
           CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    ordered_wakeup.sleep_blocked_count--;
    assert(concurrency_native_explore(
               &program,
               (ConcurrencyNativeLimits){.max_schedules = 8},
               &exhaustive) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(concurrency_native_coverage_build(
               &program, &exhaustive, &ordered_wakeup,
               (ConcurrencyNativeCoverageLimits){
                   .max_swaps_per_witness = 2,
                   .max_commutation_states = 64,
               },
               &coverage) == CONCURRENCY_NATIVE_COVERAGE_CERTIFIED);
    assert(coverage.covered_schedule_count == 6);
    concurrency_native_coverage_free(&coverage);
    concurrency_native_exploration_free(&exhaustive);
    concurrency_native_exploration_free(&ordered_wakeup);
    operations[2].reads = &shared;
    operations[2].read_count = 1;
    assert(concurrency_native_explore_race_wakeup(
               &program,
               (ConcurrencyNativeLimits){
                   .max_schedules = 4,
                   .max_commutation_states = 64,
                   .max_equivalence_states = 8,
                   .max_wakeup_obligations = 1,
               },
               &race_wakeup) == CONCURRENCY_NATIVE_LIMIT_EXCEEDED);
    assert(race_wakeup.schedules == NULL);
    operations[2].reads = NULL;
    operations[2].read_count = 0;
    concurrency_native_exploration_free(&reduced);

    operations[0].writes = NULL;
    operations[0].write_count = 0;
    operations[1].reads = NULL;
    operations[1].read_count = 0;
    QttEffectAtom effects[] = {
        {
            .kind = QTT_EFFECT_WRITE, .constructor_id = 501,
            .type_id = 601, .capability_id = 701,
            .resumption = {.finite = 1},
        },
        {
            .kind = QTT_EFFECT_READ, .constructor_id = 502,
            .type_id = 601, .capability_id = 701,
            .resumption = {.finite = 1},
        },
    };
    operations[0].effects = &effects[0];
    operations[0].effect_count = 1;
    operations[1].effects = &effects[1];
    operations[1].effect_count = 1;
    assert(concurrency_native_dependence(
               &program, process(1), process(2), NULL, 0, &dependence) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(dependence.dependent);
    assert(dependence.reasons &
           CONCURRENCY_NATIVE_DEPENDENCE_EFFECT_CAPABILITY);
    assert(concurrency_native_races(
               &program, race_schedule, 3, &races) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(races.race_count == 1);
    assert(races.races[0].reasons &
           CONCURRENCY_NATIVE_DEPENDENCE_EFFECT_CAPABILITY);
    concurrency_native_races_free(&races);
    assert(concurrency_native_explore_race_wakeup(
               &program,
               (ConcurrencyNativeLimits){
                   .max_schedules = 2,
                   .max_commutation_states = 64,
                   .max_equivalence_states = 8,
                   .max_wakeup_obligations = 8,
               },
               &race_wakeup) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(race_wakeup.schedule_count == 2);
    assert(race_wakeup.wakeup_obligation_count == 1);
    concurrency_native_exploration_free(&race_wakeup);

    ConcurrencySessionAction sender_protocol[] = {
        {CONCURRENCY_SESSION_SEND, 801},
        {CONCURRENCY_SESSION_SEND, 802},
        {CONCURRENCY_SESSION_END, 0},
    };
    ConcurrencySessionAction receiver_protocol[] = {
        {CONCURRENCY_SESSION_RECEIVE, 801},
        {CONCURRENCY_SESSION_RECEIVE, 802},
        {CONCURRENCY_SESSION_END, 0},
    };
    ConcurrencyNetworkChannel channel = {
        .channel_id = {.module_id = 41, .channel_id = 80},
        .left = {
            .endpoint_id = 810, .owner = (ConcurrencyTaskId){41, 1},
            .quantity = {.finite = 1},
            .protocol = sender_protocol, .protocol_count = 3,
        },
        .right = {
            .endpoint_id = 811, .owner = (ConcurrencyTaskId){41, 1},
            .quantity = {.finite = 1},
            .protocol = receiver_protocol, .protocol_count = 3,
        },
        .capacity = 1,
        .scope_capability_id = 812,
        .channel_type_id = 813,
    };
    ConcurrencyNetworkInitialAuthority channel_authorities[] = {
        {
            .authority = {
                .endpoint = {.module_id = 41, .endpoint_id = 810},
                .place = {.root = {.module_id = 41, .binder_id = 810}},
                .quantity = {.finite = 1}, .protocol_type_id = 814,
            },
            .owner = {41, 1},
        },
        {
            .authority = {
                .endpoint = {.module_id = 41, .endpoint_id = 811},
                .place = {.root = {.module_id = 41, .binder_id = 811}},
                .quantity = {.finite = 1}, .protocol_type_id = 815,
            },
            .owner = {41, 1},
        },
    };
    ConcurrencyNetworkTraceStep channel_steps[] = {
        {
            .channel_id = {41, 80},
            .channel_step = {
                .kind = CONCURRENCY_CHANNEL_STEP_SEND,
                .actor = {41, 1}, .endpoint_id = 810,
                .payload_type_id = 801,
            },
        },
        {
            .channel_id = {41, 80},
            .channel_step = {
                .kind = CONCURRENCY_CHANNEL_STEP_SEND,
                .actor = {41, 1}, .endpoint_id = 810,
                .payload_type_id = 802,
            },
        },
        {
            .channel_id = {41, 80},
            .channel_step = {
                .kind = CONCURRENCY_CHANNEL_STEP_RECEIVE,
                .actor = {41, 1}, .endpoint_id = 811,
                .payload_type_id = 801,
            },
        },
        {
            .channel_id = {41, 80},
            .channel_step = {
                .kind = CONCURRENCY_CHANNEL_STEP_RECEIVE,
                .actor = {41, 1}, .endpoint_id = 811,
                .payload_type_id = 802,
            },
        },
    };
    QttEffectArena *effect_arena = qtt_effect_arena_new();
    assert(effect_arena);
    QttEffectRow *channel_effects = qtt_effect_empty(effect_arena);
    QttEffectAtom receive_effect = concurrency_channel_effect_atom(
        CONCURRENCY_CHANNEL_EFFECT_RECEIVE, 812, 813);
    QttEffectAtom send_effect = concurrency_channel_effect_atom(
        CONCURRENCY_CHANNEL_EFFECT_SEND, 812, 813);
    channel_effects = qtt_effect_extend_atom(
        effect_arena, &receive_effect, channel_effects);
    channel_effects = qtt_effect_extend_atom(
        effect_arena, &send_effect, channel_effects);
    assert(channel_effects);
    QttEffectSolver *effect_solver = qtt_effect_solver_new(effect_arena);
    assert(effect_solver);
    ConcurrencyNetworkTrace network_model = {
        .channels = &channel, .channel_count = 1,
        .authorities = channel_authorities, .authority_count = 2,
        .steps = channel_steps, .step_count = 4,
        .effect_solver = effect_solver, .effects = channel_effects,
    };
    ConcurrencyNativeOperation sender_operations[] = {
        concurrency_native_network_operation(401, 0),
        concurrency_native_network_operation(402, 1),
    };
    ConcurrencyNativeOperation receiver_operations[] = {
        concurrency_native_network_operation(403, 2),
        concurrency_native_network_operation(404, 3),
    };
    ConcurrencyNativeOperation unrelated_operation =
        concurrency_native_operation(405);
    ConcurrencyNativeProcess channel_processes[] = {
        {.id = process(31), .operations = sender_operations,
         .operation_count = 2},
        {.id = process(32), .operations = receiver_operations,
         .operation_count = 2},
        {.id = process(33), .operations = &unrelated_operation,
         .operation_count = 1},
    };
    ConcurrencyNativeProgram channel_program = {
        .processes = channel_processes, .process_count = 3,
        .network_model = &network_model,
    };
    assert(concurrency_native_frontier(
               &channel_program, NULL, 0, &frontier) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(frontier.entries[0].enabled);
    assert(!frontier.entries[1].enabled);
    assert(frontier.entries[2].enabled);
    assert(frontier.enabled_count == 2);
    concurrency_native_frontier_free(&frontier);
    ConcurrencyNativeChoice after_first_send[] = {
        {.process = process(31), .local_index = 0, .tick = 0},
    };
    assert(concurrency_native_frontier(
               &channel_program, after_first_send, 1, &frontier) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(!frontier.entries[0].enabled);
    assert(frontier.entries[1].enabled);
    assert(frontier.entries[2].enabled);
    concurrency_native_frontier_free(&frontier);
    channel.capacity = 2;
    assert(concurrency_native_dependence(
               &channel_program, process(31), process(32),
               after_first_send, 1, &dependence) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(dependence.dependent);
    assert(dependence.reasons & CONCURRENCY_NATIVE_DEPENDENCE_CHANNEL);
    assert(dependence.reasons &
           CONCURRENCY_NATIVE_DEPENDENCE_EFFECT_CAPABILITY);
    channel.capacity = 1;
    assert(concurrency_native_explore(
               &channel_program,
               (ConcurrencyNativeLimits){.max_schedules = 8},
               &exploration) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(exploration.schedule_count == 5);
    assert(exploration.dead_end_count == 0);
    assert(concurrency_native_exploration_verify(
               &channel_program, &exploration) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(concurrency_native_explore_reduced(
               &channel_program, reduced_limits, &reduced) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(reduced.schedule_count == 1);
    assert(concurrency_native_coverage_build(
               &channel_program, &exploration, &reduced,
               (ConcurrencyNativeCoverageLimits){
                   .max_swaps_per_witness = 4,
                   .max_commutation_states = 64,
               },
               &coverage) == CONCURRENCY_NATIVE_COVERAGE_CERTIFIED);
    assert(coverage.covered_schedule_count == 5);
    assert(concurrency_native_coverage_verify(
               &channel_program, &exploration, &reduced, &coverage) ==
           CONCURRENCY_NATIVE_COVERAGE_CERTIFIED);
    concurrency_native_coverage_free(&coverage);
    concurrency_native_exploration_free(&reduced);
    concurrency_native_exploration_free(&exploration);

    ConcurrencySessionAction cancel_left_protocol[] = {
        {CONCURRENCY_SESSION_SEND, 901},
        {CONCURRENCY_SESSION_END, 0},
    };
    ConcurrencySessionAction cancel_right_protocol[] = {
        {CONCURRENCY_SESSION_RECEIVE, 901},
        {CONCURRENCY_SESSION_END, 0},
    };
    ConcurrencyNetworkChannel cancel_channel = {
        .channel_id = {.module_id = 41, .channel_id = 90},
        .left = {
            .endpoint_id = 910, .owner = (ConcurrencyTaskId){41, 4},
            .quantity = {.finite = 1},
            .protocol = cancel_left_protocol, .protocol_count = 2,
        },
        .right = {
            .endpoint_id = 911, .owner = (ConcurrencyTaskId){41, 1},
            .quantity = {.finite = 1},
            .protocol = cancel_right_protocol, .protocol_count = 2,
        },
        .capacity = 1,
        .scope_capability_id = 912,
        .channel_type_id = 913,
    };
    ConcurrencyNetworkInitialAuthority cancel_authorities[] = {
        {
            .authority = {
                .endpoint = {.module_id = 41, .endpoint_id = 910},
                .place = {.root = {.module_id = 41, .binder_id = 910}},
                .quantity = {.finite = 1}, .protocol_type_id = 914,
            },
            .owner = {41, 4},
        },
        {
            .authority = {
                .endpoint = {.module_id = 41, .endpoint_id = 911},
                .place = {.root = {.module_id = 41, .binder_id = 911}},
                .quantity = {.finite = 1}, .protocol_type_id = 915,
            },
            .owner = {41, 1},
        },
    };
    ConcurrencyNetworkTraceStep cancel_network_steps[] = {
        {
            .channel_id = {41, 90},
            .channel_step = {
                .kind = CONCURRENCY_CHANNEL_STEP_CANCEL,
                .actor = {41, 4}, .endpoint_id = 910,
            },
        },
    };
    QttEffectRow *cancel_effects = qtt_effect_empty(effect_arena);
    QttEffectAtom cancel_effect = concurrency_channel_effect_atom(
        CONCURRENCY_CHANNEL_EFFECT_CANCEL, 912, 913);
    cancel_effects = qtt_effect_extend_atom(
        effect_arena, &cancel_effect, cancel_effects);
    assert(cancel_effects);
    ConcurrencyNetworkTrace cancel_network_model = {
        .channels = &cancel_channel, .channel_count = 1,
        .authorities = cancel_authorities, .authority_count = 2,
        .steps = cancel_network_steps, .step_count = 1,
        .effect_solver = effect_solver, .effects = cancel_effects,
    };
    ConcurrencyNativeOperation cancel_native_root[] = {
        concurrency_native_task_operation(501, 0),
        concurrency_native_task_operation(502, 1),
        concurrency_native_task_operation(505, 4),
    };
    ConcurrencyNativeOperation cancel_native_child[] = {
        concurrency_native_task_operation(503, 2),
        concurrency_native_task_operation(504, 3),
    };
    ConcurrencyNativeOperation cancel_native_channel =
        concurrency_native_network_operation(506, 0);
    ConcurrencyNativeProcess combined_processes[] = {
        {.id = process(41), .operations = cancel_native_root,
         .operation_count = 3},
        {.id = process(42), .operations = cancel_native_child,
         .operation_count = 2},
        {.id = process(43), .operations = &cancel_native_channel,
         .operation_count = 1},
    };
    ConcurrencyNativeProgram combined_program = {
        .processes = combined_processes, .process_count = 3,
        .task_model = &cancel_model,
        .network_model = &cancel_network_model,
    };
    assert(concurrency_native_frontier(
               &combined_program, NULL, 0, &frontier) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(frontier.entries[0].enabled);
    assert(!frontier.entries[1].enabled);
    assert(!frontier.entries[2].enabled);
    concurrency_native_frontier_free(&frontier);
    ConcurrencyNativeChoice before_observation[] = {
        {.process = process(41), .local_index = 0, .tick = 0},
        {.process = process(41), .local_index = 1, .tick = 1},
    };
    assert(concurrency_native_frontier(
               &combined_program, before_observation, 2, &frontier) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(frontier.entries[1].enabled);
    assert(!frontier.entries[2].enabled);
    concurrency_native_frontier_free(&frontier);
    ConcurrencyNativeChoice before_discharge[] = {
        {.process = process(41), .local_index = 0, .tick = 0},
        {.process = process(41), .local_index = 1, .tick = 1},
        {.process = process(42), .local_index = 0, .tick = 2},
    };
    assert(concurrency_native_frontier(
               &combined_program, before_discharge, 3, &frontier) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(!frontier.entries[1].enabled);
    assert(frontier.entries[2].enabled);
    concurrency_native_frontier_free(&frontier);
    ConcurrencyNativeChoice after_discharge[] = {
        {.process = process(41), .local_index = 0, .tick = 0},
        {.process = process(41), .local_index = 1, .tick = 1},
        {.process = process(42), .local_index = 0, .tick = 2},
        {.process = process(43), .local_index = 0, .tick = 3},
    };
    assert(concurrency_native_frontier(
               &combined_program, after_discharge, 4, &frontier) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    assert(frontier.entries[1].enabled);
    concurrency_native_frontier_free(&frontier);
    assert(concurrency_native_explore(
               &combined_program,
               (ConcurrencyNativeLimits){.max_schedules = 4},
               &exploration) == CONCURRENCY_NATIVE_CERTIFIED);
    assert(exploration.schedule_count == 1);
    assert(exploration.dead_end_count > 0);
    assert(concurrency_native_exploration_verify(
               &combined_program, &exploration) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    concurrency_native_exploration_free(&exploration);
    qtt_effect_solver_free(effect_solver);
    qtt_effect_arena_free(effect_arena);

    assert(concurrency_native_frontier(&program, NULL, 0, &frontier) ==
           CONCURRENCY_NATIVE_CERTIFIED);
    frontier.entries[0].local_index++;
    assert(concurrency_native_frontier_verify(
               &program, NULL, 0, &frontier) ==
           CONCURRENCY_NATIVE_INVALID_CERTIFICATE);
    concurrency_native_frontier_free(&frontier);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "native_test.c"
            binary_path = Path(directory) / "native_test"
            source_path.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(source_path),
                    str(ROOT / "concurrency" / "native.c"),
                    str(ROOT / "concurrency" / "kernel.c"),
                    str(ROOT / "concurrency" / "dependence.c"),
                    str(ROOT / "concurrency" / "deterministic.c"),
                    str(ROOT / "concurrency" / "channel.c"),
                    str(ROOT / "concurrency" / "channel_machine.c"),
                    str(ROOT / "concurrency" / "channel_effect.c"),
                    str(ROOT / "concurrency" / "cleanup_bridge.c"),
                    str(ROOT / "concurrency" / "effect.c"),
                    str(ROOT / "concurrency" / "network.c"),
                    str(ROOT / "concurrency" / "network_trace.c"),
                    str(ROOT / "effects" / "effect.c"),
                    str(ROOT / "qtt" / "place.c"),
                    str(ROOT / "qtt" / "quantity.c"),
                    "-o", str(binary_path),
                ], check=True,
            )
            environment = os.environ.copy()
            environment["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(binary_path)], env=environment, check=True)


if __name__ == "__main__":
    unittest.main()
