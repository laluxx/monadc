#ifndef MONAD_QTT_ANF_H
#define MONAD_QTT_ANF_H

#include "demand.h"
#include "call.h"
#include "closure_policy.h"
#include "drop.h"
#include "resource.h"
#include "signature.h"
#include "signature_env.h"

typedef uint32_t QttAnfValue;
typedef uint64_t QttAnfLoanId;

typedef enum {
    QTT_ANF_TYPE_UNKNOWN,
    QTT_ANF_TYPE_UNIT,
    QTT_ANF_TYPE_NUMBER,
    QTT_ANF_TYPE_BOOL,
    QTT_ANF_TYPE_STRING,
    QTT_ANF_TYPE_AGGREGATE,
} QttAnfType;

typedef enum {
    QTT_ANF_CONST_NUMBER,
    QTT_ANF_CONST_BOOL,
    QTT_ANF_CONST_STRING,
    QTT_ANF_CONST_AGGREGATE,
    QTT_ANF_ALIAS,
    QTT_ANF_WRITE_PLACE,
    QTT_ANF_REPLACE,
    QTT_ANF_ALLOC,
    QTT_ANF_BORROW,
    QTT_ANF_END_BORROW,
    QTT_ANF_MOVE,
    QTT_ANF_MOVE_PLACE,
    QTT_ANF_DROP,
    QTT_ANF_ENV_PROJECT,
    QTT_ANF_CALL,
} QttAnfInstructionKind;

typedef enum {
    QTT_ANF_ENV_BORROW,
    QTT_ANF_ENV_MOVE,
    QTT_ANF_ENV_RELEASE,
} QttAnfEnvironmentAction;

typedef struct {
    QttAnfValue value;
    QttAnfType type;
    QttTypeId type_id;
    QttRepresentation representation;
    QttCallTransfer transfer;
    QttCoreVar source_resource;
    QttAnfLoanId loan_id;
} QttAnfCallOperand;

typedef struct {
    QttAnfInstructionKind kind;
    QttAnfValue result;
    QttAnfType result_type;
    QttAnfValue operand;
    QttCoreVar resource;
    QttAnfLoanId loan_id;
    QttAnfLoanId parent_loan_id;
    QttLoanKind loan_kind;
    QttPlace place;
    QttRepresentation representation;
    double number;
    bool boolean;
    const char *string;
    QttCoreVar result_resource;
    QttCallableId callable;
    QttAnfCallOperand *call_arguments;
    size_t call_argument_count;
    QttTypeId canonical_result_type;
    QttResultMode result_mode;
    uint64_t contract_fingerprint;
    bool owns_call_arguments;
    bool has_semantic_transition_identity;
    size_t semantic_transition_ordinal;
    bool has_semantic_call_identity;
    size_t semantic_call_ordinal;
    /* Preorder identity of an explicit typed-Core lexical loan. */
    bool has_semantic_loan_identity;
    size_t semantic_loan_ordinal;
    uint64_t environment_closure_id;
    size_t environment_ordinal;
    QttAnfEnvironmentAction environment_action;
    QttControlPathStep *control_path;
    size_t control_path_count;
} QttAnfInstruction;

typedef enum {
    QTT_ANF_RETURN,
    QTT_ANF_JUMP,
    QTT_ANF_BRANCH,
} QttAnfTerminatorKind;

typedef struct {
    QttAnfTerminatorKind kind;
    QttAnfValue value;
    size_t target;
    size_t else_target;
    /*
     * A lowering-assigned preorder identity for correspondence proofs.
     * Hand-built ANF may omit it; certified Semantic-IR bridges require it.
     */
    bool has_branch_identity;
    size_t branch_ordinal;
    QttAnfValue *arguments;
    size_t argument_count;
    QttAnfValue *else_arguments;
    size_t else_argument_count;
} QttAnfTerminator;

typedef struct {
    QttAnfValue *parameters;
    QttAnfType *parameter_types;
    /* Loan provenance is part of a block parameter's typing contract.
     * Zero denotes an ordinary value; nonzero names the live loan whose
     * borrowed value is being transferred across this CFG edge. */
    QttAnfLoanId *parameter_loans;
    size_t parameter_count;
    QttAnfInstruction *instructions;
    size_t instruction_count;
    QttAnfTerminator terminator;
} QttAnfBlock;

typedef struct {
    uint64_t closure_id;
    size_t ordinal;
    QttCoreVar source;
    QttTypeId type_id;
    QttRepresentation representation;
    QttSemanticCapability capability;
    QttClosureFieldExit exit;
    bool has_destructor;
    QttDestructorId destructor_id;
} QttAnfEnvironmentSlot;

typedef struct {
    QttPlace place;
    QttAnfType type;
} QttAnfPlaceDeclaration;

typedef struct {
    QttAnfBlock *blocks;
    size_t block_count;
    size_t entry;
    QttCoreVar *initial_resources;
    QttRepresentation *initial_representations;
    bool *initial_resource_owned;
    size_t initial_resource_count;
    QttAnfEnvironmentSlot *environment_slots;
    size_t environment_slot_count;
    QttAnfPlaceDeclaration *mutable_places;
    size_t mutable_place_count;
    const QttSignatureEnv *signatures;
} QttAnfProgram;

typedef enum {
    QTT_ANF_VALID,
    QTT_ANF_OUT_OF_MEMORY,
    QTT_ANF_INVALID_BLOCK,
    QTT_ANF_UNDEFINED_VALUE,
    QTT_ANF_DUPLICATE_VALUE,
    QTT_ANF_INVALID_RESOURCE,
    QTT_ANF_INVALID_LOAN,
    QTT_ANF_BLOCK_ARGUMENT_MISMATCH,
    QTT_ANF_OWNERSHIP_JOIN_MISMATCH,
    QTT_ANF_RESOURCE_LEAK,
    QTT_ANF_TYPE_MISMATCH,
    QTT_ANF_NON_BOOLEAN_BRANCH,
    QTT_ANF_REPRESENTATION_MISMATCH,
    QTT_ANF_INVALID_PLACE,
    QTT_ANF_UNKNOWN_CALLABLE,
    QTT_ANF_CALL_ARITY_MISMATCH,
    QTT_ANF_CALL_TYPE_MISMATCH,
    QTT_ANF_CALL_TRANSFER_MISMATCH,
    QTT_ANF_CALL_PROVENANCE_MISMATCH,
    QTT_ANF_CALL_RESULT_MISMATCH,
    QTT_ANF_CALL_CONTRACT_MISMATCH,
} QttAnfError;

typedef struct {
    QttAnfError error;
    size_t block;
    size_t instruction;
} QttAnfVerification;

typedef enum {
    QTT_ANF_VALUE_UNIT,
    QTT_ANF_VALUE_NUMBER,
    QTT_ANF_VALUE_BOOL,
    QTT_ANF_VALUE_STRING,
} QttAnfValueKind;

typedef struct {
    QttAnfValueKind kind;
    union {
        double number;
        bool boolean;
        const char *string;
    };
} QttAnfRuntimeValue;

typedef enum {
    QTT_ANF_EVAL_OK,
    QTT_ANF_EVAL_INVALID_PROGRAM,
    QTT_ANF_EVAL_OUT_OF_MEMORY,
    QTT_ANF_EVAL_NON_BOOLEAN_CONDITION,
    QTT_ANF_EVAL_STEP_LIMIT,
    QTT_ANF_EVAL_UNSUPPORTED_CALL,
    QTT_ANF_EVAL_UNSUPPORTED_ENVIRONMENT,
} QttAnfEvalError;

typedef struct {
    QttAnfEvalError error;
    QttAnfVerification verification;
    QttAnfRuntimeValue value;
    QttHeapExecution heap;
} QttAnfEvaluation;

typedef enum {
    QTT_ANF_LOWER_OK,
    QTT_ANF_LOWER_OUT_OF_MEMORY,
    QTT_ANF_LOWER_UNSUPPORTED_CORE,
    QTT_ANF_LOWER_MALFORMED_LITERAL,
    QTT_ANF_LOWER_INVALID_DEMAND,
} QttAnfLowerError;

QttAnfInstruction qtt_anf_const_number(QttAnfValue result, double value);
QttAnfInstruction qtt_anf_const_bool(QttAnfValue result, bool value);
QttAnfInstruction qtt_anf_const_string(QttAnfValue result,
                                       const char *value);
QttAnfInstruction qtt_anf_const_aggregate(QttAnfValue result);
QttAnfInstruction qtt_anf_alias(QttAnfValue result, QttAnfValue operand);
QttAnfInstruction qtt_anf_write_place(
    QttAnfValue result, QttPlace place, QttAnfValue operand);
QttAnfInstruction qtt_anf_write_place_with_loan(
    QttAnfValue result, QttPlace place, QttAnfValue operand,
    QttAnfLoanId loan_id);
QttAnfInstruction qtt_anf_replace_value(
    QttAnfValue result, QttCoreVar resource,
    QttRepresentation representation, QttAnfValue operand);
QttAnfInstruction qtt_anf_alloc(QttCoreVar resource,
                                QttRepresentation representation);
QttAnfInstruction qtt_anf_bind(QttCoreVar resource,
                               QttRepresentation representation,
                               QttAnfValue operand);
QttAnfInstruction qtt_anf_borrow(QttCoreVar resource);
QttAnfInstruction qtt_anf_borrow_value(QttAnfValue result,
                                       QttCoreVar resource);
QttAnfInstruction qtt_anf_borrow_loan(
    QttAnfValue result, QttCoreVar resource,
    QttAnfLoanId loan_id, QttLoanKind kind);
QttAnfInstruction qtt_anf_borrow_place(
    QttAnfValue result, QttPlace place,
    QttAnfLoanId loan_id, QttLoanKind kind);
QttAnfInstruction qtt_anf_reborrow_place(
    QttAnfValue result, QttPlace place, QttAnfLoanId loan_id,
    QttAnfLoanId parent_loan_id, QttLoanKind kind);
QttAnfInstruction qtt_anf_end_borrow(QttAnfLoanId loan_id);
QttAnfInstruction qtt_anf_move(QttCoreVar resource);
QttAnfInstruction qtt_anf_move_value(QttAnfValue result,
                                     QttCoreVar resource);
QttAnfInstruction qtt_anf_move_place(
    QttAnfValue result, QttAnfType result_type, QttPlace place);
QttAnfInstruction qtt_anf_drop(QttCoreVar resource);
QttAnfInstruction qtt_anf_project_environment(
    QttAnfValue result, QttAnfType result_type,
    uint64_t closure_id, size_t ordinal,
    QttCoreVar resource, QttAnfEnvironmentAction action);
QttAnfInstruction qtt_anf_call(
    QttAnfValue result,
    QttCoreVar result_resource,
    QttCallableId callable,
    QttAnfCallOperand *arguments,
    size_t argument_count,
    QttAnfType result_type,
    QttTypeId canonical_result_type,
    QttResultMode result_mode,
    QttRepresentation result_representation);
QttAnfTerminator qtt_anf_return(QttAnfValue value);
QttAnfTerminator qtt_anf_jump(size_t target, QttAnfValue *arguments,
                              size_t argument_count);
QttAnfTerminator qtt_anf_branch(QttAnfValue condition, size_t then_target,
                                size_t else_target);
QttAnfVerification qtt_anf_verify(const QttAnfProgram *program);
/* Reconstruct borrowed SSA provenance on CFG parameters and insert a missing
 * END_BORROW at the nearest provable common postdominator of every use. */
QttAnfVerification qtt_anf_infer_loan_regions(QttAnfProgram *program);
QttAnfEvaluation qtt_anf_evaluate(const QttAnfProgram *program);
QttAnfProgram *qtt_anf_lower_core(const QttCoreNode *core,
                                  QttAnfLowerError *error);
/* Lower an arbitrary typed-Core expression with the module's checked call
 * contracts available.  This is the expression analogue of
 * qtt_anf_lower_function_in_env and is required for backend certificates that
 * cross named-call boundaries. */
QttAnfProgram *qtt_anf_lower_core_in_env(
    const QttCoreNode *core,
    const QttSignatureEnv *signatures,
    uint64_t module_id,
    QttAnfLowerError *error);
QttAnfProgram *qtt_anf_lower_core_certified(
    const QttCoreNode *core,
    const QttDemandCertificate *demand,
    QttAnfLowerError *error);
QttAnfProgram *qtt_anf_lower_function(
    const QttCoreNode *lambda,
    const QttFunctionSignature *signature,
    QttAnfLowerError *error);
QttAnfProgram *qtt_anf_lower_function_in_env(
    const QttCoreNode *lambda,
    const QttFunctionSignature *signature,
    const QttSignatureEnv *signatures,
    uint64_t module_id,
    QttAnfLowerError *error);
void qtt_anf_program_free(QttAnfProgram *program);

#endif
