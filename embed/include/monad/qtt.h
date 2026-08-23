#ifndef MONAD_PUBLIC_QTT_H
#define MONAD_PUBLIC_QTT_H

#include "monad.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct monad_qtt_report monad_qtt_report_t;
typedef struct monad_compiler_environment monad_compiler_environment_t;

typedef enum monad_qtt_evidence_stage {
    /* Generalized quantitative/effect inference; no resource certificate. */
    MONAD_QTT_EVIDENCE_INFERRED = 1,
    /* Reserved for successfully checked Core/resource evidence. */
    MONAD_QTT_EVIDENCE_RESOURCE_VERIFIED = 2
} monad_qtt_evidence_stage_t;

typedef struct monad_qtt_quantity {
    uint64_t finite;
    int is_omega;
} monad_qtt_quantity_t;

typedef enum monad_qtt_representation {
    MONAD_QTT_REP_IMMEDIATE = 1,
    MONAD_QTT_REP_INLINE = 2,
    MONAD_QTT_REP_OWNED_HEAP = 3,
    MONAD_QTT_REP_FOREIGN = 4,
    MONAD_QTT_REP_UNKNOWN = 5
} monad_qtt_representation_t;

typedef struct monad_qtt_definition_evidence {
    /* Evidence is staged rather than collapsed into a boolean. Quantitative
     * usage follows Atkey's semiring judgment, while effect rows follow the
     * duplicate-label row discipline used by Koka:
     * https://bentnib.org/quantitative-type-theory.pdf
     * https://arxiv.org/abs/1406.2061
     */
    uint32_t struct_size;
    uint64_t definition_id;
    const char *name;
    monad_qtt_evidence_stage_t stage;
    int quantitative_usage_available;
    size_t parameter_usage_count;
    size_t closure_usage_count;
    int effects_complete;
    uint64_t effect_fingerprint;
    const char *portable_effect_contract;
    int resource_verified;
    int destructor_plan_available;
    const monad_qtt_quantity_t *parameter_usages;
    const monad_qtt_quantity_t *closure_usages;
    monad_qtt_representation_t result_representation;
    size_t resource_block_count;
    size_t resource_instruction_count;
    /* Deterministic index over the exact verified evidence summary; never a
     * substitute for stage/resource_verified or structural fields. Explicit
     * dup/drop resource evidence follows Perceus:
     * https://www.microsoft.com/en-us/research/publication/perceus-garbage-free-reference-counting-with-reuse/
     */
    uint64_t resource_certificate_fingerprint;
} monad_qtt_definition_evidence_t;

/* Reports and all borrowed strings are owned by their environment/source
 * result. Evidence stages prevent inferred facts from being mistaken for a
 * resource-elaboration certificate. */
MONAD_API const monad_qtt_report_t *monad_source_result_qtt_report(
    const monad_source_result_t *result);
MONAD_API const monad_qtt_report_t *monad_compiler_environment_qtt_report(
    const monad_compiler_environment_t *environment);
MONAD_API size_t monad_qtt_report_count(const monad_qtt_report_t *report);
MONAD_API const monad_qtt_definition_evidence_t *monad_qtt_report_at(
    const monad_qtt_report_t *report, size_t index);
MONAD_API const monad_qtt_definition_evidence_t *monad_qtt_report_find_id(
    const monad_qtt_report_t *report, uint64_t definition_id);
MONAD_API const monad_qtt_quantity_t *monad_qtt_definition_parameter_usage(
    const monad_qtt_definition_evidence_t *evidence, size_t index);
MONAD_API const monad_qtt_quantity_t *monad_qtt_definition_closure_usage(
    const monad_qtt_definition_evidence_t *evidence, size_t index);

#ifdef __cplusplus
}
#endif

#endif
