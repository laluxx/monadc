#ifndef MONAD_CONCURRENCY_FAILURE_H
#define MONAD_CONCURRENCY_FAILURE_H

/*
 * Portable, independently checkable causal failure evidence.
 *
 * Research basis, kept beside the trusted interface:
 * - Atkey, quantitative usage semirings (LICS 2018):
 *   https://bentnib.org/quantitative-type-theory.pdf
 * - Brady, exact-once resources and concurrent protocols in Idris 2
 *   (ECOOP 2021):
 *   https://doi.org/10.4230/LIPIcs.ECOOP.2021.9
 * - Necula, evidence production separated from a small checker (POPL 1997):
 *   https://people.eecs.berkeley.edu/~necula/Papers/pcc.pdf
 * - OpenJDK structured task ancestry and failure propagation (JEP 505):
 *   https://openjdk.org/jeps/505
 */

#include "effect.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CONCURRENCY_FAILURE_TASK,
    CONCURRENCY_FAILURE_CANCELLATION,
    CONCURRENCY_FAILURE_CLEANUP,
} ConcurrencyFailureCause;

typedef struct {
    ConcurrencyTaskId task;
    ConcurrencyFailureCause cause;
    size_t parent_index;
    size_t step_index;
    QttQuantity grade;
} ConcurrencyFailureNode;

typedef struct {
    const QttEffectSolver *solver;
    const QttEffectRow *effects;
    uint64_t scope_capability_id;
    uint64_t task_type_id;
} ConcurrencyFailureAuthority;

typedef struct {
    ConcurrencyFailureNode *nodes;
    size_t node_count;
    size_t root_count;
    QttQuantity total_grade;
    uint64_t effect_fingerprint;
    uint64_t scope_capability_id;
    uint64_t task_type_id;
    uint64_t fingerprint;
} ConcurrencyFailureCertificate;

typedef enum {
    CONCURRENCY_FAILURE_CERTIFIED,
    CONCURRENCY_FAILURE_INVALID_ARGUMENT,
    CONCURRENCY_FAILURE_INVALID_TRACE,
    CONCURRENCY_FAILURE_OUT_OF_MEMORY,
    CONCURRENCY_FAILURE_INVALID_SHAPE,
    CONCURRENCY_FAILURE_INVALID_GRADE,
    CONCURRENCY_FAILURE_INVALID_FINGERPRINT,
    CONCURRENCY_FAILURE_TRUNCATED,
    CONCURRENCY_FAILURE_UNSUPPORTED_VERSION,
} ConcurrencyFailureCertificateStatus;

ConcurrencyFailureCertificateStatus concurrency_failure_build(
    const ConcurrencyTrace *trace,
    const ConcurrencyFailureAuthority *authority,
    ConcurrencyFailureCertificate *certificate);
ConcurrencyFailureCertificateStatus concurrency_failure_verify(
    const ConcurrencyTrace *trace,
    const ConcurrencyFailureAuthority *authority,
    const ConcurrencyFailureCertificate *certificate);
size_t concurrency_failure_encoded_size(
    const ConcurrencyFailureCertificate *certificate);
ConcurrencyFailureCertificateStatus concurrency_failure_encode(
    const ConcurrencyFailureCertificate *certificate,
    uint8_t *bytes, size_t capacity, size_t *written);
ConcurrencyFailureCertificateStatus concurrency_failure_decode(
    const uint8_t *bytes, size_t byte_count,
    ConcurrencyFailureCertificate *certificate);
void concurrency_failure_free(ConcurrencyFailureCertificate *certificate);

#endif
