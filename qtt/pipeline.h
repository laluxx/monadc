#ifndef MONAD_QTT_PIPELINE_H
#define MONAD_QTT_PIPELINE_H

#include "anf.h"
#include "constraints.h"

struct InferCtx;

typedef enum {
    QTT_SOURCE_GRADES_OK,
    QTT_SOURCE_GRADES_UNSUPPORTED,
    QTT_SOURCE_GRADES_MALFORMED,
    QTT_SOURCE_GRADES_OUT_OF_MEMORY,
} QttSourceGradeStatus;

typedef struct {
    QttSourceGradeStatus status;
    QttGradeScheme *scheme;
    QttQuantity *solved;
    QttQuantity *closure_solved;
    size_t count;
    size_t closure_count;
    size_t substitutable_closure_count;
    size_t closure_domain_count;
    size_t invoked_closure_instance_count;
    size_t result_closure_count;
    size_t callable_parameter_count;
    size_t callable_domain_count;
} QttSourceGradeResult;

typedef enum {
    QTT_SHADOW_VERIFIED,
    QTT_SHADOW_UNSUPPORTED,
    QTT_SHADOW_INTERNAL_ERROR,
} QttShadowStatus;

typedef struct {
    QttShadowStatus status;
    bool core_lowered;
    bool core_validated;
    bool signature_validated;
    bool anf_lowered;
    bool verification_ran;
    QttCoreError core_error;
    QttCoreValidation core_validation;
    QttAnfLowerError lower_error;
    QttAnfError verification_error;
    size_t block_count;
    size_t instruction_count;
    size_t canonical_type_count;
    size_t graded_parameter_count;
    size_t erased_parameter_count;
    size_t parameter_count;
    size_t body_count;
} QttShadowResult;

typedef struct QttShadowDemandEvidence {
    uint64_t node_id;
    QttDemandRule rule;
    QttQuantity syntactic;
    QttQuantity runtime;
    size_t syntactic_premise_count;
    bool local_use;
    bool subject_is_binding;
    bool subject_is_parameter;
    bool direct_application;
    size_t application_argument_count;
    struct QttShadowDemandEvidence **premises;
    size_t premise_count;
} QttShadowDemandEvidence;

typedef enum {
    QTT_SHADOW_DEMAND_VALID,
    QTT_SHADOW_DEMAND_INVALID_IDENTITY,
    QTT_SHADOW_DEMAND_INVALID_SHAPE,
    QTT_SHADOW_DEMAND_INVALID_QUANTITY,
} QttShadowDemandValidation;

typedef struct {
    QttCoreVar var;
    QttQuantity allowance;
    QttQuantity observed;
    QttOwnershipMode mode;
    QttRepresentation representation;
    QttTypeId type_id;
    QttShadowDemandEvidence *demand;
} QttShadowParameterEvidence;

typedef struct {
    QttAnfInstructionKind kind;
    QttAnfValue result;
    QttAnfValue operand;
    QttCoreVar resource;
    QttAnfLoanId loan_id;
    QttAnfLoanId parent_loan_id;
    QttLoanKind loan_kind;
    QttCoreVar result_resource;
    QttRepresentation representation;
    QttCallableId callable;
    size_t call_argument_count;
    QttPlace place;
} QttShadowInstructionEvidence;

typedef struct {
    QttShadowInstructionEvidence *instructions;
    size_t instruction_count;
    QttAnfValue *parameters;
    size_t parameter_count;
    QttAnfTerminatorKind terminator;
    size_t successor_count;
    QttAnfValue terminator_value;
    size_t target;
    size_t else_target;
    QttAnfValue *arguments;
    size_t argument_count;
    QttAnfValue *else_arguments;
    size_t else_argument_count;
} QttShadowBlockEvidence;

typedef struct {
    QttShadowResult summary;
    QttShadowParameterEvidence *parameters;
    size_t parameter_count;
    QttShadowBlockEvidence *blocks;
    size_t block_count;
    QttResultMode result_mode;
    QttResultOrigin result_origin;
    QttRepresentation result_representation;
    QttTypeId result_type_id;
    uint64_t contract_fingerprint;
    bool details_complete;
} QttShadowEvidence;

/*
 * Verify one fully inferred lambda without affecting authoritative codegen.
 * The accepted fragment includes erased/consumed parameters and direct,
 * statically known lambda calls. Other valid source constructs report
 * UNSUPPORTED rather than changing authoritative compilation.
 */
QttShadowResult qtt_shadow_verify_lambda(const AST *lambda,
                                         uint64_t module_id);
QttShadowEvidence qtt_shadow_collect_lambda(const AST *lambda,
                                            uint64_t module_id);
QttShadowEvidence qtt_shadow_collect_lambda_in_env(
    const AST *lambda, uint64_t module_id,
    const QttSignatureEnv *signatures);
void qtt_shadow_evidence_free(QttShadowEvidence *evidence);
QttShadowDemandValidation qtt_shadow_demand_validate(
    const QttShadowDemandEvidence *demand);
QttGradeScheme *qtt_shadow_grade_scheme(
    const AST *lambda, uint64_t module_id);
QttSourceGradeResult qtt_source_grade_scheme(
    const AST *lambda, struct InferCtx *inference, uint64_t module_id);
void qtt_source_grade_result_free(QttSourceGradeResult *result);
const char *qtt_shadow_status_name(QttShadowStatus status);

#endif
