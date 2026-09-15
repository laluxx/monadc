#include "kernel.h"

#include <stdbool.h>
#include <stdlib.h>

typedef enum {
    TASK_RUNNING,
    TASK_CANCEL_REQUESTED,
    TASK_CLEANING,
    TASK_CLEANUP_COMPLETE,
    TASK_JOINED,
} TaskState;

typedef struct {
    ConcurrencyTaskId id;
    ConcurrencyTaskId parent;
    TaskState state;
} TaskRecord;

typedef struct {
    ConcurrencyCapability capability;
    ConcurrencyTaskId owner;
} CapabilityRecord;

typedef struct {
    ConcurrencyLoan loan;
} LoanRecord;

static bool task_equal(ConcurrencyTaskId left, ConcurrencyTaskId right) {
    return left.module_id == right.module_id && left.task_id == right.task_id;
}

static bool task_valid(ConcurrencyTaskId task) {
    return task.module_id != 0 && task.task_id != 0;
}

static bool loan_id_equal(ConcurrencyLoanId left, ConcurrencyLoanId right) {
    return left.module_id == right.module_id && left.loan_id == right.loan_id;
}

static bool loan_id_valid(ConcurrencyLoanId loan) {
    return loan.module_id != 0 && loan.loan_id != 0;
}

static ConcurrencyVerification result(
    ConcurrencyVerificationError error, size_t step_index) {
    return (ConcurrencyVerification){
        .error = error,
        .step_index = step_index,
    };
}

static size_t find_task(
    const TaskRecord *tasks, size_t task_count, ConcurrencyTaskId id) {
    for (size_t i = 0; i < task_count; i++)
        if (task_equal(tasks[i].id, id))
            return i;
    return task_count;
}

static size_t find_capability(
    const CapabilityRecord *capabilities, size_t capability_count,
    QttPlace place) {
    for (size_t i = 0; i < capability_count; i++)
        if (qtt_place_equal(capabilities[i].capability.place, place))
            return i;
    return capability_count;
}

static size_t find_loan(
    const LoanRecord *loans, size_t loan_count, ConcurrencyLoanId id) {
    for (size_t i = 0; i < loan_count; i++)
        if (loan_id_equal(loans[i].loan.id, id))
            return i;
    return loan_count;
}

static bool place_well_formed(QttPlace place) {
    if (!place.root.module_id || !place.root.binder_id ||
        place.projection_depth > QTT_PLACE_MAX_DEPTH)
        return false;
    if (!place.projection_depth)
        return true;
    if (!place.projection_id || place.projection_id != place.projection_path[0])
        return false;
    for (uint8_t i = 0; i < place.projection_depth; i++)
        if (!place.projection_path[i])
            return false;
    return true;
}

static bool capability_well_formed(ConcurrencyCapability capability) {
    QttPlace place = capability.place;
    if (!place_well_formed(place)) return false;
    if (capability.quantity.is_omega && capability.quantity.finite != 0)
        return false;
    return capability.mobility == CONCURRENCY_TRANSFERABLE ||
           capability.mobility == CONCURRENCY_THREAD_AFFINE;
}

ConcurrencyStep concurrency_spawn(
    ConcurrencyTaskId parent, ConcurrencyTaskId child,
    const QttPlace *captures, size_t capture_count) {
    return concurrency_spawn_with_loans(
        parent, child, captures, capture_count, NULL, 0);
}

ConcurrencyStep concurrency_spawn_with_loans(
    ConcurrencyTaskId parent, ConcurrencyTaskId child,
    const QttPlace *captures, size_t capture_count,
    const ConcurrencyLoanId *loan_captures, size_t loan_capture_count) {
    return (ConcurrencyStep){
        .kind = CONCURRENCY_STEP_SPAWN,
        .parent = parent,
        .child = child,
        .captures = captures,
        .capture_count = capture_count,
        .loan_captures = loan_captures,
        .loan_capture_count = loan_capture_count,
    };
}

ConcurrencyStep concurrency_join(
    ConcurrencyTaskId parent, ConcurrencyTaskId child) {
    return (ConcurrencyStep){
        .kind = CONCURRENCY_STEP_JOIN,
        .parent = parent,
        .child = child,
    };
}

ConcurrencyStep concurrency_cancel(
    ConcurrencyTaskId parent, ConcurrencyTaskId child) {
    return (ConcurrencyStep){
        .kind = CONCURRENCY_STEP_CANCEL,
        .parent = parent,
        .child = child,
    };
}

ConcurrencyStep concurrency_checkpoint(ConcurrencyTaskId task) {
    return (ConcurrencyStep){
        .kind = CONCURRENCY_STEP_CHECKPOINT,
        .child = task,
    };
}

ConcurrencyStep concurrency_fail(ConcurrencyTaskId task) {
    return (ConcurrencyStep){
        .kind = CONCURRENCY_STEP_FAIL,
        .child = task,
    };
}

ConcurrencyStep concurrency_cleanup_fail(ConcurrencyTaskId task) {
    return (ConcurrencyStep){
        .kind = CONCURRENCY_STEP_CLEANUP_FAIL,
        .child = task,
    };
}

ConcurrencyStep concurrency_cleanup_complete(ConcurrencyTaskId task) {
    return (ConcurrencyStep){
        .kind = CONCURRENCY_STEP_CLEANUP_COMPLETE,
        .child = task,
    };
}

static ConcurrencyVerification verify_initial(
    const ConcurrencyTrace *trace, CapabilityRecord *capabilities,
    LoanRecord *loans, bool *holders, size_t task_capacity) {
    if (!task_valid(trace->root) ||
        (trace->initial_capability_count && !trace->initial_capabilities) ||
        (trace->initial_loan_count && !trace->initial_loans) ||
        (trace->step_count && !trace->steps))
        return result(CONCURRENCY_MALFORMED_TRACE, 0);

    for (size_t i = 0; i < trace->initial_capability_count; i++) {
        ConcurrencyCapability capability = trace->initial_capabilities[i];
        if (!capability_well_formed(capability))
            return result(CONCURRENCY_MALFORMED_TRACE, 0);
        for (size_t j = 0; j < i; j++)
            if (qtt_place_overlaps(
                    capability.place,
                    trace->initial_capabilities[j].place))
                return result(CONCURRENCY_OVERLAPPING_AUTHORITY, 0);
        capabilities[i] = (CapabilityRecord){
            .capability = capability,
            .owner = trace->root,
        };
    }
    for (size_t i = 0; i < trace->initial_loan_count; i++) {
        ConcurrencyLoan loan = trace->initial_loans[i];
        if (!loan_id_valid(loan.id) || !place_well_formed(loan.place) ||
            (loan.kind != QTT_LOAN_SHARED &&
             loan.kind != QTT_LOAN_EXCLUSIVE) ||
            (loan.mobility != CONCURRENCY_TRANSFERABLE &&
             loan.mobility != CONCURRENCY_THREAD_AFFINE))
            return result(CONCURRENCY_MALFORMED_TRACE, 0);
        if (find_loan(loans, i, loan.id) != i)
            return result(CONCURRENCY_MALFORMED_TRACE, 0);
        bool covered = false;
        for (size_t capability = 0;
             capability < trace->initial_capability_count; capability++)
            if (qtt_place_contains(
                    capabilities[capability].capability.place, loan.place)) {
                covered = true;
                break;
            }
        if (!covered)
            return result(CONCURRENCY_UNKNOWN_CAPABILITY, 0);
        for (size_t previous = 0; previous < i; previous++)
            if (qtt_place_overlaps(loans[previous].loan.place, loan.place) &&
                (loans[previous].loan.kind == QTT_LOAN_EXCLUSIVE ||
                 loan.kind == QTT_LOAN_EXCLUSIVE))
                return result(CONCURRENCY_LOAN_CONFLICT, 0);
        loans[i].loan = loan;
        holders[i * task_capacity] = true;
    }
    return result(CONCURRENCY_VALID, 0);
}

static bool capability_is_borrowed(
    QttPlace place, const LoanRecord *loans, size_t loan_count) {
    for (size_t i = 0; i < loan_count; i++)
        if (qtt_place_overlaps(place, loans[i].loan.place))
            return true;
    return false;
}

static bool loan_owner_is_transferable(
    QttPlace place, const CapabilityRecord *capabilities,
    size_t capability_count) {
    for (size_t i = 0; i < capability_count; i++)
        if (qtt_place_contains(capabilities[i].capability.place, place))
            return capabilities[i].capability.mobility ==
                   CONCURRENCY_TRANSFERABLE;
    return false;
}

static ConcurrencyVerification verify_spawn(
    const ConcurrencyStep *step, size_t step_index,
    TaskRecord *tasks, size_t *task_count, size_t task_capacity,
    CapabilityRecord *capabilities, size_t capability_count,
    LoanRecord *loans, size_t loan_count, bool *holders) {
    size_t parent = find_task(tasks, *task_count, step->parent);
    if (parent == *task_count || tasks[parent].state != TASK_RUNNING)
        return result(CONCURRENCY_UNKNOWN_TASK, step_index);
    if (!task_valid(step->child) ||
        (step->capture_count && !step->captures) ||
        (step->loan_capture_count && !step->loan_captures))
        return result(CONCURRENCY_MALFORMED_TRACE, step_index);
    if (find_task(tasks, *task_count, step->child) != *task_count)
        return result(CONCURRENCY_DUPLICATE_TASK, step_index);
    if (*task_count == task_capacity)
        return result(CONCURRENCY_OUT_OF_MEMORY, step_index);

    for (size_t i = 0; i < step->capture_count; i++) {
        size_t selected = find_capability(
            capabilities, capability_count, step->captures[i]);
        if (selected == capability_count ||
            !task_equal(capabilities[selected].owner, step->parent))
            return result(CONCURRENCY_UNKNOWN_CAPABILITY, step_index);
        if (capability_is_borrowed(
                step->captures[i], loans, loan_count))
            return result(CONCURRENCY_BORROWED_CAPABILITY, step_index);
        if (capabilities[selected].capability.mobility !=
            CONCURRENCY_TRANSFERABLE)
            return result(CONCURRENCY_NOT_TRANSFERABLE, step_index);
        if (!qtt_quantity_equal(
                capabilities[selected].capability.quantity,
                qtt_quantity_finite(1)))
            return result(CONCURRENCY_NON_LINEAR_CAPTURE, step_index);
        for (size_t j = 0; j < i; j++)
            if (qtt_place_equal(step->captures[i], step->captures[j]))
                return result(CONCURRENCY_UNKNOWN_CAPABILITY, step_index);
    }

    for (size_t i = 0; i < step->loan_capture_count; i++) {
        size_t selected = find_loan(
            loans, loan_count, step->loan_captures[i]);
        if (selected == loan_count)
            return result(CONCURRENCY_UNKNOWN_LOAN, step_index);
        if (!holders[selected * task_capacity + parent])
            return result(CONCURRENCY_LOAN_NOT_HELD, step_index);
        if (loans[selected].loan.mobility != CONCURRENCY_TRANSFERABLE ||
            !loan_owner_is_transferable(
                loans[selected].loan.place,
                capabilities, capability_count))
            return result(CONCURRENCY_LOAN_NOT_TRANSFERABLE, step_index);
        for (size_t j = 0; j < i; j++)
            if (loan_id_equal(
                    step->loan_captures[i], step->loan_captures[j]))
                return result(CONCURRENCY_LOAN_CONFLICT, step_index);
    }

    tasks[*task_count] = (TaskRecord){
        .id = step->child,
        .parent = step->parent,
        .state = TASK_RUNNING,
    };
    (*task_count)++;
    for (size_t i = 0; i < step->capture_count; i++) {
        size_t selected = find_capability(
            capabilities, capability_count, step->captures[i]);
        capabilities[selected].owner = step->child;
    }
    size_t child = *task_count - 1;
    for (size_t i = 0; i < step->loan_capture_count; i++) {
        size_t selected = find_loan(
            loans, loan_count, step->loan_captures[i]);
        if (loans[selected].loan.kind == QTT_LOAN_EXCLUSIVE)
            holders[selected * task_capacity + parent] = false;
        holders[selected * task_capacity + child] = true;
    }
    return result(CONCURRENCY_VALID, step_index);
}

static ConcurrencyVerification verify_join(
    const ConcurrencyStep *step, size_t step_index,
    TaskRecord *tasks, size_t task_count,
    CapabilityRecord *capabilities, size_t capability_count,
    size_t loan_count, bool *holders, size_t task_capacity) {
    size_t child = find_task(tasks, task_count, step->child);
    if (child == task_count)
        return result(CONCURRENCY_UNKNOWN_TASK, step_index);
    if (tasks[child].state == TASK_JOINED)
        return result(CONCURRENCY_ALREADY_JOINED, step_index);
    if (tasks[child].state == TASK_CANCEL_REQUESTED)
        return result(CONCURRENCY_CANCELLATION_NOT_OBSERVED, step_index);
    if (tasks[child].state == TASK_CLEANING)
        return result(CONCURRENCY_CLEANUP_INCOMPLETE, step_index);
    if (!task_equal(tasks[child].parent, step->parent))
        return result(CONCURRENCY_WRONG_PARENT, step_index);
    size_t parent = find_task(tasks, task_count, step->parent);
    if (parent == task_count ||
        (tasks[parent].state != TASK_RUNNING &&
         tasks[parent].state != TASK_CLEANING))
        return result(CONCURRENCY_UNKNOWN_TASK, step_index);
    for (size_t i = 0; i < task_count; i++)
        if (tasks[i].state != TASK_JOINED &&
            task_equal(tasks[i].parent, step->child))
            return result(CONCURRENCY_LIVE_CHILD, step_index);

    for (size_t i = 0; i < capability_count; i++)
        if (task_equal(capabilities[i].owner, step->child))
            capabilities[i].owner = step->parent;
    for (size_t i = 0; i < loan_count; i++)
        if (holders[i * task_capacity + child]) {
            holders[i * task_capacity + child] = false;
            holders[i * task_capacity + parent] = true;
        }
    tasks[child].state = TASK_JOINED;
    return result(CONCURRENCY_VALID, step_index);
}

static ConcurrencyVerification verify_cancel(
    const ConcurrencyStep *step, size_t step_index,
    TaskRecord *tasks, size_t task_count) {
    size_t child = find_task(tasks, task_count, step->child);
    if (child == task_count)
        return result(CONCURRENCY_UNKNOWN_TASK, step_index);
    if (tasks[child].state == TASK_JOINED)
        return result(CONCURRENCY_ALREADY_JOINED, step_index);
    if (!task_equal(tasks[child].parent, step->parent))
        return result(CONCURRENCY_WRONG_PARENT, step_index);
    size_t parent = find_task(tasks, task_count, step->parent);
    if (parent == task_count ||
        (tasks[parent].state != TASK_RUNNING &&
         tasks[parent].state != TASK_CLEANING))
        return result(CONCURRENCY_UNKNOWN_TASK, step_index);
    if (tasks[child].state == TASK_RUNNING)
        tasks[child].state = TASK_CANCEL_REQUESTED;
    return result(CONCURRENCY_VALID, step_index);
}

static ConcurrencyVerification verify_checkpoint(
    const ConcurrencyStep *step, size_t step_index,
    TaskRecord *tasks, size_t task_count) {
    size_t task = find_task(tasks, task_count, step->child);
    if (task == task_count || tasks[task].state == TASK_JOINED)
        return result(CONCURRENCY_UNKNOWN_TASK, step_index);
    if (tasks[task].state == TASK_CANCEL_REQUESTED)
        tasks[task].state = TASK_CLEANING;
    return result(CONCURRENCY_VALID, step_index);
}

static ConcurrencyVerification verify_cleanup_complete(
    const ConcurrencyStep *step, size_t step_index,
    TaskRecord *tasks, size_t task_count) {
    size_t task = find_task(tasks, task_count, step->child);
    if (task == task_count || tasks[task].state == TASK_JOINED)
        return result(CONCURRENCY_UNKNOWN_TASK, step_index);
    if (tasks[task].state != TASK_CLEANING)
        return result(CONCURRENCY_INVALID_CLEANUP, step_index);
    for (size_t i = 0; i < task_count; i++)
        if (tasks[i].state != TASK_JOINED &&
            task_equal(tasks[i].parent, step->child))
            return result(CONCURRENCY_LIVE_CHILD, step_index);
    tasks[task].state = TASK_CLEANUP_COMPLETE;
    return result(CONCURRENCY_VALID, step_index);
}

static ConcurrencyVerification verify_fail(
    const ConcurrencyStep *step, size_t step_index,
    TaskRecord *tasks, size_t task_count) {
    size_t task = find_task(tasks, task_count, step->child);
    if (task == task_count || tasks[task].state == TASK_JOINED)
        return result(CONCURRENCY_UNKNOWN_TASK, step_index);
    if (tasks[task].state != TASK_RUNNING)
        return result(CONCURRENCY_INVALID_FAILURE, step_index);
    tasks[task].state = TASK_CLEANING;
    return result(CONCURRENCY_VALID, step_index);
}

static ConcurrencyVerification verify_cleanup_fail(
    const ConcurrencyStep *step, size_t step_index,
    TaskRecord *tasks, size_t task_count) {
    size_t task = find_task(tasks, task_count, step->child);
    if (task == task_count || tasks[task].state == TASK_JOINED)
        return result(CONCURRENCY_UNKNOWN_TASK, step_index);
    if (tasks[task].state != TASK_CLEANING)
        return result(CONCURRENCY_INVALID_FAILURE, step_index);
    return result(CONCURRENCY_VALID, step_index);
}

static ConcurrencyVerification verify_trace(
    const ConcurrencyTrace *trace, bool require_closed) {
    if (!trace)
        return result(CONCURRENCY_MALFORMED_TRACE, 0);
    if (trace->step_count == SIZE_MAX ||
        trace->step_count + 1 > SIZE_MAX / sizeof(TaskRecord))
        return result(CONCURRENCY_OUT_OF_MEMORY, 0);
    if (trace->initial_capability_count >
            SIZE_MAX / sizeof(CapabilityRecord) ||
        trace->initial_loan_count > SIZE_MAX / sizeof(LoanRecord))
        return result(CONCURRENCY_OUT_OF_MEMORY, 0);

    size_t task_capacity = trace->step_count + 1;
    TaskRecord *tasks = calloc(task_capacity, sizeof(*tasks));
    CapabilityRecord *capabilities = calloc(
        trace->initial_capability_count ? trace->initial_capability_count : 1,
        sizeof(*capabilities));
    LoanRecord *loans = calloc(
        trace->initial_loan_count ? trace->initial_loan_count : 1,
        sizeof(*loans));
    if (trace->initial_loan_count &&
        task_capacity > SIZE_MAX / trace->initial_loan_count) {
        free(tasks);
        free(capabilities);
        free(loans);
        return result(CONCURRENCY_OUT_OF_MEMORY, 0);
    }
    size_t holder_count = trace->initial_loan_count * task_capacity;
    bool *holders = calloc(holder_count ? holder_count : 1, sizeof(*holders));
    if (!tasks || !capabilities || !loans || !holders) {
        free(tasks);
        free(capabilities);
        free(loans);
        free(holders);
        return result(CONCURRENCY_OUT_OF_MEMORY, 0);
    }

    ConcurrencyVerification verification = verify_initial(
        trace, capabilities, loans, holders, task_capacity);
    size_t primary_failure_count = 0;
    size_t cleanup_failure_count = 0;
    size_t task_count = 1;
    tasks[0] = (TaskRecord){
        .id = trace->root,
        .state = TASK_RUNNING,
    };
    for (size_t i = 0;
         verification.error == CONCURRENCY_VALID && i < trace->step_count;
         i++) {
        const ConcurrencyStep *step = &trace->steps[i];
        if (step->kind == CONCURRENCY_STEP_SPAWN)
            verification = verify_spawn(
                step, i, tasks, &task_count, task_capacity,
                capabilities, trace->initial_capability_count,
                loans, trace->initial_loan_count, holders);
        else if (step->kind == CONCURRENCY_STEP_JOIN)
            verification = verify_join(
                step, i, tasks, task_count,
                capabilities, trace->initial_capability_count,
                trace->initial_loan_count, holders, task_capacity);
        else if (step->kind == CONCURRENCY_STEP_CANCEL)
            verification = verify_cancel(
                step, i, tasks, task_count);
        else if (step->kind == CONCURRENCY_STEP_CHECKPOINT)
        {
            size_t task = find_task(tasks, task_count, step->child);
            bool observes_cancellation = task != task_count &&
                tasks[task].state == TASK_CANCEL_REQUESTED;
            verification = verify_checkpoint(step, i, tasks, task_count);
            if (verification.error == CONCURRENCY_VALID &&
                observes_cancellation)
                primary_failure_count++;
        }
        else if (step->kind == CONCURRENCY_STEP_FAIL) {
            verification = verify_fail(step, i, tasks, task_count);
            if (verification.error == CONCURRENCY_VALID)
                primary_failure_count++;
        }
        else if (step->kind == CONCURRENCY_STEP_CLEANUP_FAIL) {
            verification = verify_cleanup_fail(step, i, tasks, task_count);
            if (verification.error == CONCURRENCY_VALID)
                cleanup_failure_count++;
        }
        else if (step->kind == CONCURRENCY_STEP_CLEANUP_COMPLETE)
            verification = verify_cleanup_complete(
                step, i, tasks, task_count);
        else
            verification = result(CONCURRENCY_MALFORMED_TRACE, i);
    }
    if (require_closed && verification.error == CONCURRENCY_VALID)
        for (size_t i = 1; i < task_count; i++)
            if (tasks[i].state != TASK_JOINED) {
                ConcurrencyVerificationError error = CONCURRENCY_LIVE_CHILD;
                if (tasks[i].state == TASK_CANCEL_REQUESTED)
                    error = CONCURRENCY_CANCELLATION_NOT_OBSERVED;
                else if (tasks[i].state == TASK_CLEANING)
                    error = CONCURRENCY_CLEANUP_INCOMPLETE;
                verification = result(error, trace->step_count);
                break;
            }

    verification.primary_failure_count = primary_failure_count;
    verification.cleanup_failure_count = cleanup_failure_count;

    free(tasks);
    free(capabilities);
    free(loans);
    free(holders);
    return verification;
}

ConcurrencyVerification concurrency_verify_trace(const ConcurrencyTrace *trace) {
    return verify_trace(trace, true);
}

ConcurrencyVerification concurrency_verify_trace_prefix(
    const ConcurrencyTrace *trace) {
    return verify_trace(trace, false);
}
