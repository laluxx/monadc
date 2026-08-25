"""Executable contracts for Cycles 1 and 2 of the concurrency proof kernel."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ConcurrencyKernelTests(unittest.TestCase):
    def test_structured_spawn_join_conserves_linear_authority(self):
        source = r'''
#include "concurrency/kernel.h"

#include <assert.h>

static ConcurrencyTaskId task(uint64_t id) {
    return (ConcurrencyTaskId){.module_id = 7, .task_id = id};
}

static QttPlace place(uint64_t binder) {
    return qtt_place_root((QttCoreVar){
        .module_id = 7,
        .binder_id = binder,
    });
}

static ConcurrencyCapability capability(
    uint64_t binder, QttQuantity quantity, ConcurrencyMobility mobility) {
    return (ConcurrencyCapability){
        .place = place(binder),
        .quantity = quantity,
        .mobility = mobility,
    };
}

static ConcurrencyLoanId loan_id(uint64_t id) {
    return (ConcurrencyLoanId){.module_id = 7, .loan_id = id};
}

static ConcurrencyLoan loan(
    uint64_t id, uint64_t binder, QttLoanKind kind,
    ConcurrencyMobility mobility) {
    return (ConcurrencyLoan){
        .id = loan_id(id),
        .place = place(binder),
        .kind = kind,
        .mobility = mobility,
    };
}

static void expect(
    ConcurrencyTrace trace, ConcurrencyVerificationError expected) {
    ConcurrencyVerification result = concurrency_verify_trace(&trace);
    assert(result.error == expected);
}

int main(void) {
    ConcurrencyCapability initial[] = {
        capability(10, qtt_quantity_finite(1),
                   CONCURRENCY_TRANSFERABLE),
        capability(20, qtt_quantity_finite(1),
                   CONCURRENCY_THREAD_AFFINE),
        capability(30, qtt_quantity_omega(),
                   CONCURRENCY_TRANSFERABLE),
    };
    QttPlace transfer[] = {place(10)};
    ConcurrencyStep complete[] = {
        concurrency_spawn(task(1), task(2), transfer, 1),
        concurrency_join(task(1), task(2)),
    };
    ConcurrencyTrace trace = {
        .root = task(1),
        .initial_capabilities = initial,
        .initial_capability_count = 3,
        .steps = complete,
        .step_count = 2,
    };
    expect(trace, CONCURRENCY_VALID);

    ConcurrencyStep live[] = {
        concurrency_spawn(task(1), task(2), transfer, 1),
    };
    trace.steps = live;
    trace.step_count = 1;
    expect(trace, CONCURRENCY_LIVE_CHILD);
    assert(concurrency_verify_trace_prefix(&trace).error == CONCURRENCY_VALID);

    QttPlace affine[] = {place(20)};
    complete[0] = concurrency_spawn(task(1), task(2), affine, 1);
    trace.steps = complete;
    trace.step_count = 2;
    expect(trace, CONCURRENCY_NOT_TRANSFERABLE);

    QttPlace unrestricted[] = {place(30)};
    complete[0] = concurrency_spawn(task(1), task(2), unrestricted, 1);
    expect(trace, CONCURRENCY_NON_LINEAR_CAPTURE);

    QttPlace forged[] = {place(99)};
    complete[0] = concurrency_spawn(task(1), task(2), forged, 1);
    expect(trace, CONCURRENCY_UNKNOWN_CAPABILITY);

    complete[0] = concurrency_spawn(task(1), task(2), transfer, 1);
    complete[1] = concurrency_join(task(3), task(2));
    expect(trace, CONCURRENCY_WRONG_PARENT);

    ConcurrencyStep double_join[] = {
        concurrency_spawn(task(1), task(2), transfer, 1),
        concurrency_join(task(1), task(2)),
        concurrency_join(task(1), task(2)),
    };
    trace.steps = double_join;
    trace.step_count = 3;
    expect(trace, CONCURRENCY_ALREADY_JOINED);

    ConcurrencyCapability overlap[] = {
        capability(10, qtt_quantity_finite(1),
                   CONCURRENCY_TRANSFERABLE),
        capability(10, qtt_quantity_finite(1),
                   CONCURRENCY_TRANSFERABLE),
    };
    trace.initial_capabilities = overlap;
    trace.initial_capability_count = 2;
    trace.steps = NULL;
    trace.step_count = 0;
    expect(trace, CONCURRENCY_OVERLAPPING_AUTHORITY);

    ConcurrencyCapability malformed[] = {
        capability(10, qtt_quantity_finite(1),
                   (ConcurrencyMobility)99),
    };
    trace.initial_capabilities = malformed;
    trace.initial_capability_count = 1;
    expect(trace, CONCURRENCY_MALFORMED_TRACE);

    ConcurrencyCapability borrowed_owner[] = {
        capability(40, qtt_quantity_finite(1),
                   CONCURRENCY_TRANSFERABLE),
    };
    ConcurrencyLoan shared[] = {
        loan(1, 40, QTT_LOAN_SHARED, CONCURRENCY_TRANSFERABLE),
    };
    ConcurrencyLoanId shared_capture[] = {loan_id(1)};
    ConcurrencyStep shared_siblings[] = {
        concurrency_spawn_with_loans(
            task(1), task(2), NULL, 0, shared_capture, 1),
        concurrency_spawn_with_loans(
            task(1), task(3), NULL, 0, shared_capture, 1),
        concurrency_join(task(1), task(2)),
        concurrency_join(task(1), task(3)),
    };
    trace.initial_capabilities = borrowed_owner;
    trace.initial_capability_count = 1;
    trace.initial_loans = shared;
    trace.initial_loan_count = 1;
    trace.steps = shared_siblings;
    trace.step_count = 4;
    expect(trace, CONCURRENCY_VALID);

    QttPlace borrowed_place[] = {place(40)};
    ConcurrencyStep move_borrowed[] = {
        concurrency_spawn(task(1), task(2), borrowed_place, 1),
        concurrency_join(task(1), task(2)),
    };
    trace.steps = move_borrowed;
    trace.step_count = 2;
    expect(trace, CONCURRENCY_BORROWED_CAPABILITY);

    shared[0].mobility = CONCURRENCY_THREAD_AFFINE;
    trace.steps = shared_siblings;
    trace.step_count = 4;
    expect(trace, CONCURRENCY_LOAN_NOT_TRANSFERABLE);
    shared[0].mobility = CONCURRENCY_TRANSFERABLE;

    ConcurrencyLoanId forged_loan[] = {loan_id(99)};
    ConcurrencyStep forged_borrow[] = {
        concurrency_spawn_with_loans(
            task(1), task(2), NULL, 0, forged_loan, 1),
        concurrency_join(task(1), task(2)),
    };
    trace.steps = forged_borrow;
    trace.step_count = 2;
    expect(trace, CONCURRENCY_UNKNOWN_LOAN);

    ConcurrencyLoan exclusive[] = {
        loan(2, 40, QTT_LOAN_EXCLUSIVE, CONCURRENCY_TRANSFERABLE),
    };
    ConcurrencyLoanId exclusive_capture[] = {loan_id(2)};
    ConcurrencyStep exclusive_reuse[] = {
        concurrency_spawn_with_loans(
            task(1), task(2), NULL, 0, exclusive_capture, 1),
        concurrency_spawn_with_loans(
            task(1), task(3), NULL, 0, exclusive_capture, 1),
        concurrency_join(task(1), task(2)),
        concurrency_join(task(1), task(3)),
    };
    trace.initial_loans = exclusive;
    trace.steps = exclusive_reuse;
    trace.step_count = 4;
    expect(trace, CONCURRENCY_LOAN_NOT_HELD);

    ConcurrencyStep exclusive_nested[] = {
        concurrency_spawn_with_loans(
            task(1), task(2), NULL, 0, exclusive_capture, 1),
        concurrency_spawn_with_loans(
            task(2), task(3), NULL, 0, exclusive_capture, 1),
        concurrency_join(task(2), task(3)),
        concurrency_join(task(1), task(2)),
    };
    trace.steps = exclusive_nested;
    expect(trace, CONCURRENCY_VALID);

    ConcurrencyLoan conflicting[] = {
        loan(3, 40, QTT_LOAN_SHARED, CONCURRENCY_TRANSFERABLE),
        loan(4, 40, QTT_LOAN_EXCLUSIVE, CONCURRENCY_TRANSFERABLE),
    };
    trace.initial_loans = conflicting;
    trace.initial_loan_count = 2;
    trace.steps = NULL;
    trace.step_count = 0;
    expect(trace, CONCURRENCY_LOAN_CONFLICT);

    trace.initial_capabilities = borrowed_owner;
    trace.initial_capability_count = 1;
    trace.initial_loans = NULL;
    trace.initial_loan_count = 0;
    ConcurrencyStep cancelled[] = {
        concurrency_spawn(task(1), task(2), borrowed_place, 1),
        concurrency_cancel(task(1), task(2)),
        concurrency_cancel(task(1), task(2)),
        concurrency_checkpoint(task(2)),
        concurrency_cancel(task(1), task(2)),
        concurrency_cleanup_complete(task(2)),
        concurrency_join(task(1), task(2)),
    };
    trace.steps = cancelled;
    trace.step_count = 7;
    expect(trace, CONCURRENCY_VALID);

    ConcurrencyStep unobserved[] = {
        concurrency_spawn(task(1), task(2), borrowed_place, 1),
        concurrency_cancel(task(1), task(2)),
        concurrency_join(task(1), task(2)),
    };
    trace.steps = unobserved;
    trace.step_count = 3;
    expect(trace, CONCURRENCY_CANCELLATION_NOT_OBSERVED);

    ConcurrencyStep unclean[] = {
        concurrency_spawn(task(1), task(2), borrowed_place, 1),
        concurrency_cancel(task(1), task(2)),
        concurrency_checkpoint(task(2)),
        concurrency_join(task(1), task(2)),
    };
    trace.steps = unclean;
    trace.step_count = 4;
    expect(trace, CONCURRENCY_CLEANUP_INCOMPLETE);

    ConcurrencyStep premature_cleanup[] = {
        concurrency_spawn(task(1), task(2), borrowed_place, 1),
        concurrency_cleanup_complete(task(2)),
        concurrency_join(task(1), task(2)),
    };
    trace.steps = premature_cleanup;
    trace.step_count = 3;
    expect(trace, CONCURRENCY_INVALID_CLEANUP);

    ConcurrencyStep wrong_canceller[] = {
        concurrency_spawn(task(1), task(2), borrowed_place, 1),
        concurrency_cancel(task(3), task(2)),
        concurrency_join(task(1), task(2)),
    };
    trace.steps = wrong_canceller;
    expect(trace, CONCURRENCY_WRONG_PARENT);

    ConcurrencyStep recursive_cleanup[] = {
        concurrency_spawn(task(1), task(2), borrowed_place, 1),
        concurrency_spawn(task(2), task(3), borrowed_place, 1),
        concurrency_cancel(task(1), task(2)),
        concurrency_checkpoint(task(2)),
        concurrency_cancel(task(2), task(3)),
        concurrency_checkpoint(task(3)),
        concurrency_cleanup_complete(task(3)),
        concurrency_join(task(2), task(3)),
        concurrency_cleanup_complete(task(2)),
        concurrency_join(task(1), task(2)),
    };
    trace.steps = recursive_cleanup;
    trace.step_count = 10;
    expect(trace, CONCURRENCY_VALID);

    recursive_cleanup[8] = concurrency_cleanup_complete(task(2));
    recursive_cleanup[7] = concurrency_cleanup_complete(task(2));
    trace.step_count = 9;
    expect(trace, CONCURRENCY_LIVE_CHILD);

    ConcurrencyStep exceptional_cleanup[] = {
        concurrency_spawn(task(1), task(2), borrowed_place, 1),
        concurrency_fail(task(2)),
        concurrency_cleanup_fail(task(2)),
        concurrency_cleanup_complete(task(2)),
        concurrency_join(task(1), task(2)),
    };
    trace.steps = exceptional_cleanup;
    trace.step_count = 5;
    ConcurrencyVerification failures = concurrency_verify_trace(&trace);
    assert(failures.error == CONCURRENCY_VALID);
    assert(failures.primary_failure_count == 1);
    assert(failures.cleanup_failure_count == 1);

    exceptional_cleanup[1] = concurrency_cleanup_fail(task(2));
    trace.step_count = 5;
    expect(trace, CONCURRENCY_INVALID_FAILURE);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "kernel_test.c"
            binary_path = Path(directory) / "kernel_test"
            source_path.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(source_path),
                    str(ROOT / "concurrency" / "kernel.c"),
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
