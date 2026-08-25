#ifndef MONAD_CONCURRENCY_KERNEL_H
#define MONAD_CONCURRENCY_KERNEL_H

/*
 * Independently checkable structured-concurrency authority transitions.
 * Quantitative resource tracking:
 * https://bentnib.org/quantitative-type-theory.pdf
 * Structured task lifetime and parent-child failure propagation:
 * https://openjdk.org/jeps/505
 * Nursery-style lexical concurrency motivation:
 * https://vorpus.org/blog/notes-on-structured-concurrency-or-go-statement-considered-harmful/
 */

#include "../qtt/place.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t module_id;
    uint64_t task_id;
} ConcurrencyTaskId;

typedef enum {
    CONCURRENCY_TRANSFERABLE,
    CONCURRENCY_THREAD_AFFINE,
} ConcurrencyMobility;

typedef struct {
    QttPlace place;
    QttQuantity quantity;
    ConcurrencyMobility mobility;
} ConcurrencyCapability;

typedef struct {
    uint64_t module_id;
    uint64_t loan_id;
} ConcurrencyLoanId;

/* A QTT loan plus the independent proof that it may cross a task edge. */
typedef struct {
    ConcurrencyLoanId id;
    QttPlace place;
    QttLoanKind kind;
    ConcurrencyMobility mobility;
} ConcurrencyLoan;

typedef enum {
    CONCURRENCY_STEP_SPAWN,
    CONCURRENCY_STEP_JOIN,
    CONCURRENCY_STEP_CANCEL,
    CONCURRENCY_STEP_CHECKPOINT,
    CONCURRENCY_STEP_FAIL,
    CONCURRENCY_STEP_CLEANUP_FAIL,
    CONCURRENCY_STEP_CLEANUP_COMPLETE,
} ConcurrencyStepKind;

typedef struct {
    ConcurrencyStepKind kind;
    ConcurrencyTaskId parent;
    ConcurrencyTaskId child;
    const QttPlace *captures;
    size_t capture_count;
    const ConcurrencyLoanId *loan_captures;
    size_t loan_capture_count;
} ConcurrencyStep;

typedef struct {
    ConcurrencyTaskId root;
    const ConcurrencyCapability *initial_capabilities;
    size_t initial_capability_count;
    const ConcurrencyLoan *initial_loans;
    size_t initial_loan_count;
    const ConcurrencyStep *steps;
    size_t step_count;
} ConcurrencyTrace;

typedef enum {
    CONCURRENCY_VALID,
    CONCURRENCY_MALFORMED_TRACE,
    CONCURRENCY_OUT_OF_MEMORY,
    CONCURRENCY_DUPLICATE_TASK,
    CONCURRENCY_UNKNOWN_TASK,
    CONCURRENCY_UNKNOWN_CAPABILITY,
    CONCURRENCY_NOT_TRANSFERABLE,
    CONCURRENCY_NON_LINEAR_CAPTURE,
    CONCURRENCY_OVERLAPPING_AUTHORITY,
    CONCURRENCY_WRONG_PARENT,
    CONCURRENCY_ALREADY_JOINED,
    CONCURRENCY_LIVE_CHILD,
    CONCURRENCY_UNKNOWN_LOAN,
    CONCURRENCY_LOAN_NOT_HELD,
    CONCURRENCY_LOAN_NOT_TRANSFERABLE,
    CONCURRENCY_LOAN_CONFLICT,
    CONCURRENCY_BORROWED_CAPABILITY,
    CONCURRENCY_CANCELLATION_NOT_OBSERVED,
    CONCURRENCY_CLEANUP_INCOMPLETE,
    CONCURRENCY_INVALID_CLEANUP,
    CONCURRENCY_INVALID_FAILURE,
} ConcurrencyVerificationError;

typedef struct {
    ConcurrencyVerificationError error;
    size_t step_index;
    size_t primary_failure_count;
    size_t cleanup_failure_count;
} ConcurrencyVerification;

ConcurrencyStep concurrency_spawn(
    ConcurrencyTaskId parent, ConcurrencyTaskId child,
    const QttPlace *captures, size_t capture_count);
ConcurrencyStep concurrency_spawn_with_loans(
    ConcurrencyTaskId parent, ConcurrencyTaskId child,
    const QttPlace *captures, size_t capture_count,
    const ConcurrencyLoanId *loan_captures, size_t loan_capture_count);
ConcurrencyStep concurrency_join(
    ConcurrencyTaskId parent, ConcurrencyTaskId child);
ConcurrencyStep concurrency_cancel(
    ConcurrencyTaskId parent, ConcurrencyTaskId child);
ConcurrencyStep concurrency_checkpoint(ConcurrencyTaskId task);
ConcurrencyStep concurrency_fail(ConcurrencyTaskId task);
ConcurrencyStep concurrency_cleanup_fail(ConcurrencyTaskId task);
ConcurrencyStep concurrency_cleanup_complete(ConcurrencyTaskId task);

ConcurrencyVerification concurrency_verify_trace(const ConcurrencyTrace *trace);
/* Replay an open operational prefix without requiring every child to be
 * joined yet. All authority, loan, lifetime, cancellation, and cleanup
 * transitions are checked identically to complete-trace verification. */
ConcurrencyVerification concurrency_verify_trace_prefix(
    const ConcurrencyTrace *trace);

#endif
