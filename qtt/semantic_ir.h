#ifndef MONAD_QTT_SEMANTIC_IR_H
#define MONAD_QTT_SEMANTIC_IR_H

#include "core_usage.h"
#include "core_effect.h"
#include "call.h"
#include "closure_policy.h"
#include "drop.h"
#include "effect.h"
#include "resource.h"
#include "signature_env.h"
#include "type_identity.h"

typedef enum {
    QTT_SEMANTIC_GRADE_RESULT_DEMAND,
    QTT_SEMANTIC_GRADE_ALIAS_TRANSFER,
    QTT_SEMANTIC_GRADE_CONSTRUCTION,
    QTT_SEMANTIC_GRADE_INVOCATION,
} QttSemanticGradeRole;

/*
 * A representation says how a value is physically carried.  A capability
 * says what the current program is permitted to do with the resource.
 * Keeping these orthogonal prevents "heap pointer" from silently meaning
 * "uniquely owned".
 */
typedef struct {
    size_t node_index;
    uint64_t module_id;
    uint64_t binder_id;
    QttSemanticGradeRole role;
    QttGradeExpr *expression;
} QttSemanticGradeEvidence;

typedef struct {
    QttCoreVar var;
    QttTypeId type_id;
    QttDestructorDescriptor *descriptor;
    QttDropMaskCertificate *mask;
} QttSemanticDestructorEvidence;

typedef struct {
    size_t node_index;
    uint64_t closure_id;
    size_t ordinal;
    QttCoreVar source;
    QttTypeId type_id;
    const Type *type; /* borrowed from typed Core */
    QttRepresentation representation;
    QttClosureStorage storage;
    QttClosureFieldExit exit;
    QttDestructorDescriptor *descriptor;
} QttSemanticClosureFieldEvidence;

typedef struct {
    size_t ordinal;
    QttResourceOpKind kind;
    QttCoreVar var;
    QttCoreVar target;
    QttPlace place;
    QttRepresentation representation;
    QttLoanKind loan_kind;
    QttSemanticCapability before;
    QttSemanticCapability after;
    QttSemanticCapability target_after;
    QttControlPathStep *control_path;
    size_t control_path_count;
} QttSemanticCapabilityTransition;

typedef enum {
    QTT_SEMANTIC_EDGE_THEN,
    QTT_SEMANTIC_EDGE_ELSE,
} QttSemanticEdgeArm;

typedef struct {
    size_t branch_ordinal;
    QttSemanticEdgeArm arm;
    size_t transition_offset;
    size_t transition_count;
    size_t entry_live_count;
    size_t exit_live_count;
    uint64_t entry_context_fingerprint;
    uint64_t exit_context_fingerprint;
} QttSemanticCapabilityEdge;

typedef struct {
    size_t node_index;
    size_t call_ordinal;
    QttCallableId callable;
    uint64_t contract_fingerprint;
    QttCallTransfer *argument_transfers;
    QttRepresentation *argument_representations;
    QttTypeId *argument_type_ids;
    QttNominalAuthority *argument_nominal_authorities;
    QttCoreVar *source_vars;
    size_t argument_count;
    QttResultContract result;
    QttCoreVar result_resource;
    QttControlPathStep *control_path;
    size_t control_path_count;
} QttSemanticCallEvidence;

QttCoreVar qtt_semantic_owned_result_resource(
    uint64_t module_id, size_t call_ordinal);

typedef struct {
    QttCoreKind kind;
    const QttCoreNode *source;
    size_t subtree_size;
    QttCoreVar var;
    /* Exact lexical parent for QTT_CORE_BORROW; zero denotes root. */
    QttCoreVar parent_loan;
    QttPlace place;
    QttLoanKind loan_kind;
    QttTypeId type_id;
    QttSemanticGradeRole grade_role;
    QttEffectRow *effects;
    QttRepresentation representation;
    QttSemanticCapability capability;
    /* Nominal algebraic-effect authority, independent of value ownership. */
    uint64_t effect_constructor_id;
    uint64_t effect_capability_id;
    size_t grade_offset;
    size_t grade_count;
    uint64_t closure_id;
    QttClosurePolicyEvidence closure_policy;
    uint64_t instance_id;
    uint64_t callee_closure_id;
    size_t domain_count;
} QttSemanticNode;

typedef struct QttSemanticFunction QttSemanticFunction;

/* Portable identity of the proof-bearing root judgment
 *     Sigma ; Psi | Gamma |- t : A ! epsilon ; Phi
 * carried independently of the mutable solver allocation. */
typedef struct {
    bool present;
    uint64_t authority_fingerprint;
    uint64_t row_fingerprint;
    uint64_t constraint_fingerprint;
    size_t constraint_count;
    QttEffectConstraintResult constraint_result;
} QttSemanticEffectJudgment;

typedef enum {
    QTT_SEMANTIC_IR_OK,
    QTT_SEMANTIC_IR_INVALID_CORE,
    QTT_SEMANTIC_IR_UNSUPPORTED,
    QTT_SEMANTIC_IR_OUT_OF_MEMORY,
} QttSemanticIrError;

typedef enum {
    QTT_SEMANTIC_IR_VALID,
    QTT_SEMANTIC_IR_SOURCE_MISMATCH,
    QTT_SEMANTIC_IR_SHAPE_MISMATCH,
    QTT_SEMANTIC_IR_KIND_MISMATCH,
    QTT_SEMANTIC_IR_PROVENANCE_MISMATCH,
    QTT_SEMANTIC_IR_TYPE_MISMATCH,
    QTT_SEMANTIC_IR_GRADE_ROLE_MISMATCH,
    QTT_SEMANTIC_IR_EFFECT_MISMATCH,
    QTT_SEMANTIC_IR_EFFECT_AUTHORITY_MISMATCH,
    QTT_SEMANTIC_IR_GRADE_EVIDENCE_MISMATCH,
    QTT_SEMANTIC_IR_CALL_EVIDENCE_MISMATCH,
    QTT_SEMANTIC_IR_REPRESENTATION_MISMATCH,
    QTT_SEMANTIC_IR_CAPABILITY_MISMATCH,
    QTT_SEMANTIC_IR_RESOURCE_EVIDENCE_MISMATCH,
    QTT_SEMANTIC_IR_DESTRUCTOR_EVIDENCE_MISMATCH,
    QTT_SEMANTIC_IR_CAPABILITY_TRANSITION_MISMATCH,
    QTT_SEMANTIC_IR_CAPABILITY_EDGE_MISMATCH,
    QTT_SEMANTIC_IR_CLOSURE_POLICY_MISMATCH,
    QTT_SEMANTIC_IR_CLOSURE_FIELD_MISMATCH,
} QttSemanticIrValidation;

QttSemanticFunction *qtt_semantic_ir_lower(
    const QttCoreNode *core, QttSemanticIrError *error);
QttSemanticFunction *qtt_semantic_ir_lower_certified(
    const QttCoreNode *core, const QttCoreEffectResult *effects,
    QttSemanticIrError *error);
QttSemanticFunction *qtt_semantic_ir_lower_in_env(
    const QttCoreNode *core, const QttSignatureEnv *signatures,
    uint64_t module_id, QttSemanticIrError *error);
QttSemanticIrValidation qtt_semantic_ir_verify(
    const QttSemanticFunction *function, const QttCoreNode *core);
size_t qtt_semantic_ir_node_count(const QttSemanticFunction *function);
QttSemanticNode *qtt_semantic_ir_nodes(QttSemanticFunction *function);
size_t qtt_semantic_ir_effect_label_count(
    const QttSemanticFunction *function, size_t node_index,
    const char *label);
/* Read-only, execution-order diagnostic projection of effectful Core nodes.
 * The returned text is owned by the caller. It is not executable authority;
 * consumers must verify the Semantic IR and its effect judgment separately. */
char *qtt_semantic_ir_effect_trace_format(
    const QttSemanticFunction *function);
size_t qtt_semantic_ir_evidence_count(
    const QttSemanticFunction *function);
QttSemanticGradeEvidence *qtt_semantic_ir_evidence(
    QttSemanticFunction *function);
QttResourceBlock *qtt_semantic_ir_resources(
    QttSemanticFunction *function);
QttResourceElaborationError qtt_semantic_ir_resource_status(
    const QttSemanticFunction *function);
size_t qtt_semantic_ir_destructor_count(
    const QttSemanticFunction *function);
QttSemanticDestructorEvidence *qtt_semantic_ir_destructors(
    QttSemanticFunction *function);
size_t qtt_semantic_ir_transition_count(
    const QttSemanticFunction *function);
QttSemanticCapabilityTransition *qtt_semantic_ir_transitions(
    QttSemanticFunction *function);
size_t qtt_semantic_ir_edge_count(
    const QttSemanticFunction *function);
QttSemanticCapabilityEdge *qtt_semantic_ir_edges(
    QttSemanticFunction *function);
size_t qtt_semantic_ir_call_count(
    const QttSemanticFunction *function);
QttSemanticCallEvidence *qtt_semantic_ir_calls(
    QttSemanticFunction *function);
size_t qtt_semantic_ir_closure_field_count(
    const QttSemanticFunction *function);
QttSemanticClosureFieldEvidence *qtt_semantic_ir_closure_fields(
    QttSemanticFunction *function);
QttQuantity qtt_semantic_ir_solved_usage(
    QttSemanticFunction *function, uint64_t binder_id);
bool qtt_semantic_ir_solved_usage_for(
    QttSemanticFunction *function, QttCoreVar var,
    QttQuantity *quantity);
const QttUsageContext *qtt_semantic_ir_usage(
    const QttSemanticFunction *function);
const QttCoreNode *qtt_semantic_ir_source(
    const QttSemanticFunction *function);
const QttSignatureEnv *qtt_semantic_ir_signatures(
    const QttSemanticFunction *function);
uint64_t qtt_semantic_ir_module_id(
    const QttSemanticFunction *function);
const QttSemanticEffectJudgment *qtt_semantic_ir_effect_judgment(
    const QttSemanticFunction *function);
bool qtt_semantic_ir_verify_effect_judgment(
    const QttSemanticFunction *function, const QttCoreNode *core,
    const QttCoreEffectResult *effects);
void qtt_semantic_ir_free(QttSemanticFunction *function);

#endif
