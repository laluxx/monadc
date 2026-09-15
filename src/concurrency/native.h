#ifndef MONAD_CONCURRENCY_NATIVE_H
#define MONAD_CONCURRENCY_NATIVE_H

/*
 * Native process-owned operational frontier.  Unlike the Cycle 5 binary
 * compatibility model, enabledness is derived only from each process cursor.
 * QTT places and the existing effect atoms supply semantic footprints.
 *
 * Event structures: https://doi.org/10.1007/3-540-17906-2_31
 * DPOR:             https://doi.org/10.1145/1040305.1040315
 * Optimal DPOR:     https://doi.org/10.1145/3073408
 * Wakeup algorithm: https://user.it.uu.se/~parosha/publications/papers/popl2014.pdf
 * Concurrency map:  https://ncatlab.org/nlab/show/concurrency+theory#idea
 *
 * Cycle 5.29 audits every race-driven obligation through prefix-derived sleep
 * sets, ordered insert[E], and certified sleep transfer before exposing the
 * integrated result. Recursive Algorithm 2 control remains a later boundary.
 */

#include "process.h"
#include "../effects/effect.h"

typedef struct {
    uint64_t operation_id;
    bool task_transition;
    size_t task_step_index;
    bool network_transition;
    size_t network_step_index;
    const QttPlace *reads;
    size_t read_count;
    const QttPlace *writes;
    size_t write_count;
    const QttEffectAtom *effects;
    size_t effect_count;
} ConcurrencyNativeOperation;

typedef struct {
    ConcurrencyProcessId id;
    const ConcurrencyNativeOperation *operations;
    size_t operation_count;
} ConcurrencyNativeProcess;

typedef struct {
    const ConcurrencyNativeProcess *processes;
    size_t process_count;
    const ConcurrencyTrace *task_model;
    const ConcurrencyNetworkTrace *network_model;
} ConcurrencyNativeProgram;

typedef struct {
    ConcurrencyProcessId process;
    size_t local_index;
    uint64_t tick;
} ConcurrencyNativeChoice;

typedef struct {
    ConcurrencyProcessId process;
    size_t local_index;
    uint64_t operation_id;
    bool enabled;
} ConcurrencyNativeFrontierEntry;

typedef struct {
    ConcurrencyNativeFrontierEntry *entries;
    size_t process_count;
    size_t enabled_count;
    size_t prefix_count;
    uint64_t program_fingerprint;
    uint64_t prefix_fingerprint;
    uint64_t fingerprint;
} ConcurrencyNativeFrontier;

typedef enum {
    CONCURRENCY_NATIVE_DEPENDENCE_NONE = 0,
    CONCURRENCY_NATIVE_DEPENDENCE_PROCESS_ORDER = 1u << 0,
    CONCURRENCY_NATIVE_DEPENDENCE_QTT_PLACE = 1u << 1,
    CONCURRENCY_NATIVE_DEPENDENCE_EFFECT_CAPABILITY = 1u << 2,
    CONCURRENCY_NATIVE_DEPENDENCE_TASK_ORDER = 1u << 3,
    CONCURRENCY_NATIVE_DEPENDENCE_TASK_LIFETIME = 1u << 4,
    CONCURRENCY_NATIVE_DEPENDENCE_CLEANUP = 1u << 5,
    CONCURRENCY_NATIVE_DEPENDENCE_AUTHORITY = 1u << 6,
    CONCURRENCY_NATIVE_DEPENDENCE_CHANNEL = 1u << 7,
} ConcurrencyNativeDependenceReason;

typedef struct {
    bool dependent;
    uint32_t reasons;
} ConcurrencyNativeDependence;

typedef struct {
    ConcurrencyProcessId process;
    bool retained;
    uint32_t reasons;
} ConcurrencyNativeSleepDecision;

typedef struct {
    ConcurrencyProcessId selected;
    ConcurrencyNativeSleepDecision *decisions;
    size_t input_count;
    ConcurrencyProcessId *retained;
    size_t retained_count;
    uint64_t program_fingerprint;
    uint64_t prefix_fingerprint;
    uint64_t input_fingerprint;
    uint64_t fingerprint;
} ConcurrencyNativeSleepStepCertificate;

typedef struct {
    size_t earlier_index;
    size_t later_index;
    uint32_t reasons;
    size_t reversal_prefix_count;
    ConcurrencyNativeChoice *reversal;
    size_t reversal_count;
    uint64_t fingerprint;
} ConcurrencyNativeRace;

typedef struct {
    size_t occurrence_count;
    unsigned char *happens_before;
    size_t happens_before_edge_count;
    ConcurrencyNativeRace *races;
    size_t race_count;
    uint64_t program_fingerprint;
    uint64_t schedule_fingerprint;
    uint64_t fingerprint;
} ConcurrencyNativeRaceCertificate;

typedef enum {
    CONCURRENCY_NATIVE_COMMUTATION_CERTIFIED,
    CONCURRENCY_NATIVE_COMMUTATION_INVALID_ARGUMENT,
    CONCURRENCY_NATIVE_COMMUTATION_DEPENDENT,
    CONCURRENCY_NATIVE_COMMUTATION_NOT_COENABLED,
    CONCURRENCY_NATIVE_COMMUTATION_DOES_NOT_COMMUTE,
    CONCURRENCY_NATIVE_COMMUTATION_LIMIT_EXCEEDED,
    CONCURRENCY_NATIVE_COMMUTATION_OUT_OF_MEMORY,
    CONCURRENCY_NATIVE_COMMUTATION_INVALID_CERTIFICATE,
} ConcurrencyNativeCommutationStatus;

typedef struct {
    size_t prefix_count;
    size_t max_states;
    ConcurrencyProcessId left;
    ConcurrencyProcessId right;
    uint64_t left_operation_id;
    uint64_t right_operation_id;
    size_t forward_state_count;
    size_t reverse_state_count;
    uint64_t forward_state_fingerprint;
    uint64_t reverse_state_fingerprint;
    uint64_t program_fingerprint;
    uint64_t prefix_fingerprint;
    uint64_t fingerprint;
} ConcurrencyNativeCommutationCertificate;

typedef struct {
    size_t max_schedules;
    size_t max_commutation_states;
    size_t max_equivalence_states;
    size_t max_wakeup_obligations;
} ConcurrencyNativeLimits;

typedef struct {
    ConcurrencyNativeLimits limits;
    size_t prefix_count;
    size_t left_count;
    size_t right_count;
    bool related;
    size_t reached_state_index;
    size_t *swap_positions;
    size_t swap_count;
    uint64_t program_fingerprint;
    uint64_t prefix_fingerprint;
    uint64_t left_fingerprint;
    uint64_t right_fingerprint;
    uint64_t fingerprint;
} ConcurrencyNativeInitialCertificate;

typedef struct {
    ConcurrencyNativeLimits limits;
    size_t prefix_count;
    size_t left_count;
    size_t right_count;
    bool related;
    size_t left_schedule_index;
    size_t right_schedule_index;
    size_t *swap_positions;
    size_t swap_count;
    uint64_t program_fingerprint;
    uint64_t prefix_fingerprint;
    uint64_t left_fingerprint;
    uint64_t right_fingerprint;
    uint64_t fingerprint;
} ConcurrencyNativeWeakInitialCertificate;

typedef struct {
    ConcurrencyProcessId process;
    ConcurrencyNativeWeakInitialCertificate weak_initial;
} ConcurrencyNativeWakeupAdmissibilityDecision;

typedef struct {
    ConcurrencyNativeLimits limits;
    ConcurrencyNativeWakeupAdmissibilityDecision *decisions;
    size_t decision_count;
    bool admissible;
    size_t blocking_sleep_index;
    size_t prefix_count;
    size_t sequence_count;
    uint64_t program_fingerprint;
    uint64_t prefix_fingerprint;
    uint64_t sequence_fingerprint;
    uint64_t sleep_fingerprint;
    uint64_t fingerprint;
} ConcurrencyNativeWakeupAdmissibilityCertificate;

typedef enum {
    CONCURRENCY_NATIVE_EXPLORATION_EXHAUSTIVE,
    CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_REDUCED,
    CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_WAKEUP_TREE,
    CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ONLINE_WAKEUP_TREE,
    CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_RACE_WAKEUP,
    CONCURRENCY_NATIVE_EXPLORATION_CERTIFIED_ORDERED_WAKEUP,
} ConcurrencyNativeExplorationMode;

typedef struct {
    ConcurrencyNativeChoice *choices;
    size_t choice_count;
    uint64_t schedule_fingerprint;
} ConcurrencyNativeSchedule;

typedef struct {
    size_t parent_index;
    ConcurrencyNativeChoice choice;
    size_t depth;
    bool terminal;
} ConcurrencyNativeWakeupNode;

typedef struct {
    ConcurrencyNativeLimits limits;
    ConcurrencyNativeWakeupNode *nodes;
    size_t node_count;
    size_t before_node_count;
    bool suppressed;
    size_t anchor_node_index;
    size_t added_leaf_node_index;
    size_t prefix_count;
    size_t candidate_count;
    size_t sleep_count;
    size_t existing_leaf_count;
    uint64_t program_fingerprint;
    uint64_t input_fingerprint;
    uint64_t fingerprint;
} ConcurrencyNativeWakeupInsertCertificate;

typedef struct {
    ConcurrencyNativeChoice *prefix;
    size_t prefix_count;
    ConcurrencyNativeChoice *sequence;
    size_t sequence_count;
    bool discharged;
    uint64_t source_schedule_fingerprint;
    uint64_t race_fingerprint;
    uint64_t fingerprint;
} ConcurrencyNativeWakeupObligation;

typedef struct {
    ConcurrencyNativeExplorationMode mode;
    ConcurrencyNativeLimits limits;
    ConcurrencyNativeSchedule *schedules;
    size_t schedule_count;
    size_t branch_count;
    size_t dead_end_count;
    size_t commutation_count;
    size_t pruned_choice_count;
    size_t fallback_branch_count;
    size_t equivalence_class_count;
    size_t examined_schedule_count;
    size_t redundant_schedule_count;
    size_t ordered_insertion_count;
    size_t suppressed_insertion_count;
    size_t sleep_transition_count;
    size_t sleep_blocked_count;
    ConcurrencyNativeWakeupNode *wakeup_nodes;
    size_t wakeup_node_count;
    ConcurrencyNativeWakeupObligation *wakeup_obligations;
    size_t wakeup_obligation_count;
    size_t discharged_wakeup_obligation_count;
    uint64_t program_fingerprint;
    uint64_t fingerprint;
} ConcurrencyNativeExploration;

typedef struct {
    size_t max_swaps_per_witness;
    size_t max_commutation_states;
} ConcurrencyNativeCoverageLimits;

typedef struct {
    size_t exhaustive_schedule_index;
    size_t representative_schedule_index;
    uint64_t exhaustive_schedule_fingerprint;
    uint64_t representative_schedule_fingerprint;
    size_t *swap_positions;
    size_t swap_count;
} ConcurrencyNativeCoverageWitness;

typedef struct {
    ConcurrencyNativeCoverageLimits limits;
    uint64_t exhaustive_exploration_fingerprint;
    uint64_t reduced_exploration_fingerprint;
    ConcurrencyNativeCoverageWitness *witnesses;
    size_t witness_count;
    size_t covered_schedule_count;
    size_t total_swap_count;
    uint64_t fingerprint;
} ConcurrencyNativeCoverage;

typedef enum {
    CONCURRENCY_NATIVE_COVERAGE_CERTIFIED,
    CONCURRENCY_NATIVE_COVERAGE_INVALID_ARGUMENT,
    CONCURRENCY_NATIVE_COVERAGE_INVALID_EXPLORATION,
    CONCURRENCY_NATIVE_COVERAGE_UNCOVERED_SCHEDULE,
    CONCURRENCY_NATIVE_COVERAGE_LIMIT_EXCEEDED,
    CONCURRENCY_NATIVE_COVERAGE_OUT_OF_MEMORY,
    CONCURRENCY_NATIVE_COVERAGE_INVALID_CERTIFICATE,
} ConcurrencyNativeCoverageStatus;

typedef enum {
    CONCURRENCY_NATIVE_CERTIFIED,
    CONCURRENCY_NATIVE_INVALID_ARGUMENT,
    CONCURRENCY_NATIVE_INVALID_PROGRAM,
    CONCURRENCY_NATIVE_MALFORMED_PREFIX,
    CONCURRENCY_NATIVE_LIMIT_EXCEEDED,
    CONCURRENCY_NATIVE_OUT_OF_MEMORY,
    CONCURRENCY_NATIVE_INVALID_CERTIFICATE,
} ConcurrencyNativeStatus;

ConcurrencyNativeOperation concurrency_native_operation(uint64_t operation_id);
ConcurrencyNativeOperation concurrency_native_task_operation(
    uint64_t operation_id, size_t task_step_index);
ConcurrencyNativeOperation concurrency_native_network_operation(
    uint64_t operation_id, size_t network_step_index);
ConcurrencyNativeStatus concurrency_native_frontier(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    ConcurrencyNativeFrontier *frontier);
ConcurrencyNativeStatus concurrency_native_frontier_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeFrontier *frontier);
void concurrency_native_frontier_free(ConcurrencyNativeFrontier *frontier);
ConcurrencyNativeStatus concurrency_native_dependence(
    const ConcurrencyNativeProgram *program,
    ConcurrencyProcessId left, ConcurrencyProcessId right,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    ConcurrencyNativeDependence *dependence);
ConcurrencyNativeStatus concurrency_native_sleep_step(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    ConcurrencyProcessId selected,
    ConcurrencyNativeSleepStepCertificate *certificate);
ConcurrencyNativeStatus concurrency_native_sleep_step_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    ConcurrencyProcessId selected,
    const ConcurrencyNativeSleepStepCertificate *certificate);
void concurrency_native_sleep_step_free(
    ConcurrencyNativeSleepStepCertificate *certificate);
ConcurrencyNativeStatus concurrency_native_races(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *schedule, size_t schedule_count,
    ConcurrencyNativeRaceCertificate *certificate);
bool concurrency_native_happens_before(
    const ConcurrencyNativeRaceCertificate *certificate,
    size_t earlier_index, size_t later_index);
ConcurrencyNativeStatus concurrency_native_races_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *schedule, size_t schedule_count,
    const ConcurrencyNativeRaceCertificate *certificate);
void concurrency_native_races_free(
    ConcurrencyNativeRaceCertificate *certificate);
ConcurrencyNativeStatus concurrency_native_weak_initial(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *left, size_t left_count,
    const ConcurrencyNativeChoice *right, size_t right_count,
    ConcurrencyNativeLimits limits,
    ConcurrencyNativeWeakInitialCertificate *certificate);
ConcurrencyNativeStatus concurrency_native_weak_initial_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *left, size_t left_count,
    const ConcurrencyNativeChoice *right, size_t right_count,
    const ConcurrencyNativeWeakInitialCertificate *certificate);
void concurrency_native_weak_initial_free(
    ConcurrencyNativeWeakInitialCertificate *certificate);
ConcurrencyNativeStatus concurrency_native_initial(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *left, size_t left_count,
    const ConcurrencyNativeChoice *right, size_t right_count,
    ConcurrencyNativeLimits limits,
    ConcurrencyNativeInitialCertificate *certificate);
ConcurrencyNativeStatus concurrency_native_initial_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *left, size_t left_count,
    const ConcurrencyNativeChoice *right, size_t right_count,
    const ConcurrencyNativeInitialCertificate *certificate);
void concurrency_native_initial_free(
    ConcurrencyNativeInitialCertificate *certificate);
ConcurrencyNativeStatus concurrency_native_wakeup_admissible(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *sequence, size_t sequence_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    ConcurrencyNativeLimits limits,
    ConcurrencyNativeWakeupAdmissibilityCertificate *certificate);
ConcurrencyNativeStatus concurrency_native_wakeup_admissible_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeChoice *sequence, size_t sequence_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    const ConcurrencyNativeWakeupAdmissibilityCertificate *certificate);
void concurrency_native_wakeup_admissibility_free(
    ConcurrencyNativeWakeupAdmissibilityCertificate *certificate);
ConcurrencyNativeStatus concurrency_native_wakeup_insert(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    const ConcurrencyNativeSchedule *existing_leaves, size_t existing_leaf_count,
    const ConcurrencyNativeChoice *candidate, size_t candidate_count,
    ConcurrencyNativeLimits limits,
    ConcurrencyNativeWakeupInsertCertificate *certificate);
ConcurrencyNativeStatus concurrency_native_wakeup_insert_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyProcessId *sleep, size_t sleep_count,
    const ConcurrencyNativeSchedule *existing_leaves, size_t existing_leaf_count,
    const ConcurrencyNativeChoice *candidate, size_t candidate_count,
    const ConcurrencyNativeWakeupInsertCertificate *certificate);
void concurrency_native_wakeup_insert_free(
    ConcurrencyNativeWakeupInsertCertificate *certificate);
ConcurrencyNativeCommutationStatus concurrency_native_commute(
    const ConcurrencyNativeProgram *program,
    ConcurrencyProcessId left, ConcurrencyProcessId right,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    size_t max_states,
    ConcurrencyNativeCommutationCertificate *certificate);
ConcurrencyNativeCommutationStatus concurrency_native_commutation_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeChoice *prefix, size_t prefix_count,
    const ConcurrencyNativeCommutationCertificate *certificate);
ConcurrencyNativeStatus concurrency_native_explore(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExploration *exploration);
ConcurrencyNativeStatus concurrency_native_explore_reduced(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExploration *exploration);
ConcurrencyNativeStatus concurrency_native_explore_wakeup_tree(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExploration *exploration);
ConcurrencyNativeStatus concurrency_native_explore_wakeup_tree_online(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExploration *exploration);
ConcurrencyNativeStatus concurrency_native_explore_race_wakeup(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExploration *exploration);
ConcurrencyNativeStatus concurrency_native_explore_ordered_wakeup(
    const ConcurrencyNativeProgram *program, ConcurrencyNativeLimits limits,
    ConcurrencyNativeExploration *exploration);
ConcurrencyNativeStatus concurrency_native_exploration_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeExploration *exploration);
void concurrency_native_exploration_free(
    ConcurrencyNativeExploration *exploration);
ConcurrencyNativeCoverageStatus concurrency_native_coverage_build(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeExploration *exhaustive,
    const ConcurrencyNativeExploration *reduced,
    ConcurrencyNativeCoverageLimits limits,
    ConcurrencyNativeCoverage *coverage);
ConcurrencyNativeCoverageStatus concurrency_native_coverage_verify(
    const ConcurrencyNativeProgram *program,
    const ConcurrencyNativeExploration *exhaustive,
    const ConcurrencyNativeExploration *reduced,
    const ConcurrencyNativeCoverage *coverage);
void concurrency_native_coverage_free(ConcurrencyNativeCoverage *coverage);

#endif
