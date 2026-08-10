#ifndef MONAD_QTT_RESOURCE_H
#define MONAD_QTT_RESOURCE_H

#include "place.h"

struct QttSignatureEnv;

/*
 * Resource IR exposes the structural rules hidden by ordinary source terms.
 * Verification is a linear state transition Γ --op--> Γ': allocation adds
 * one live capability, borrow preserves it, and move/drop consume it.
 * Conditional arms must produce extensionally equal capability contexts.
 *
 * This follows the explicit dup/drop direction used by Perceus:
 * https://www.microsoft.com/en-us/research/publication/perceus-garbage-free-reference-counting-with-reuse/
 */

typedef enum {
    QTT_REP_IMMEDIATE,
    QTT_REP_INLINE,
    QTT_REP_OWNED_HEAP,
    QTT_REP_FOREIGN,
    QTT_REP_UNKNOWN,
} QttRepresentation;

/*
 * Representation describes storage; capability describes permission.
 * This vocabulary is shared by Semantic IR and every verified lowering.
 */
typedef enum {
    QTT_SEMANTIC_CAPABILITY_NONE,
    QTT_SEMANTIC_CAPABILITY_OWNED,
    QTT_SEMANTIC_CAPABILITY_BORROWED,
    QTT_SEMANTIC_CAPABILITY_SHARED,
    QTT_SEMANTIC_CAPABILITY_ERASED,
    QTT_SEMANTIC_CAPABILITY_UNKNOWN,
} QttSemanticCapability;

typedef enum {
    QTT_RESOURCE_ALLOC,
    QTT_RESOURCE_BORROW,
    /* Proof-only binding: var denotes the already-live target capability. */
    QTT_RESOURCE_ALIAS,
    /* Close the scoped alias proof without changing physical ownership. */
    QTT_RESOURCE_END_ALIAS,
    QTT_RESOURCE_MOVE,
    /* Evacuate a certified subplace while retaining the aggregate root. */
    QTT_RESOURCE_MOVE_PLACE,
    /* Transfer target through an active alias proof. */
    QTT_RESOURCE_MOVE_ALIAS,
    /* Mutation proof: alias must be an active exclusive loan of target. */
    QTT_RESOURCE_WRITE_ALIAS,
    QTT_RESOURCE_DROP,
    QTT_RESOURCE_DUP,
    QTT_RESOURCE_REHOME,
    /*
     * Atomically destroy the live owned payload and install a fresh payload
     * under the same capability identity.
     */
    QTT_RESOURCE_REPLACE,
    QTT_RESOURCE_BRANCH,
} QttResourceOpKind;

typedef struct {
    size_t branch_ordinal;
    bool else_arm;
} QttControlPathStep;

typedef struct QttResourceBlock QttResourceBlock;

typedef struct {
    QttResourceOpKind kind;
    QttCoreVar var;
    QttCoreVar target;
    /* Nonzero only for a reborrow: names the active parent alias. */
    QttCoreVar parent_alias;
    /*
     * Canonical authority identity for alias-family operations. target is
     * retained as the physical root projection consumed by existing backends.
     */
    QttPlace place;
    QttRepresentation representation;
    QttLoanKind loan_kind;
    struct {
        QttResourceBlock *then_block;
        QttResourceBlock *else_block;
    } branch;
} QttResourceOp;

struct QttResourceBlock {
    QttResourceOp *ops;
    size_t count;
};

typedef enum {
    QTT_RESOURCE_VALID,
    QTT_RESOURCE_OUT_OF_MEMORY,
    QTT_RESOURCE_UNKNOWN_VAR,
    QTT_RESOURCE_DUPLICATE_ALLOC,
    QTT_RESOURCE_USE_AFTER_CONSUME,
    QTT_RESOURCE_DOUBLE_CONSUME,
    QTT_RESOURCE_BRANCH_MISMATCH,
    QTT_RESOURCE_LEAK,
    QTT_RESOURCE_BRANCH_CHOICE_MISSING,
    QTT_RESOURCE_INVALID_CAPABILITY,
    QTT_RESOURCE_DUPLICATE_ALIAS,
    QTT_RESOURCE_UNKNOWN_ALIAS,
    QTT_RESOURCE_ALIAS_TARGET_MISMATCH,
    QTT_RESOURCE_LOAN_KIND_MISMATCH,
    QTT_RESOURCE_LOAN_CONFLICT,
    QTT_RESOURCE_ACCESS_CONFLICT,
    QTT_RESOURCE_PLAN_MISMATCH,
} QttResourceError;

typedef struct {
    QttResourceError error;
    size_t operation_index;
    QttCoreVar var;
    size_t live_count;
} QttResourceVerification;

typedef enum {
    QTT_RESOURCE_ELABORATE_OK,
    QTT_RESOURCE_ELABORATE_OUT_OF_MEMORY,
    QTT_RESOURCE_ELABORATE_UNSUPPORTED_CORE,
    QTT_RESOURCE_ELABORATE_UNBALANCED_CONTROL_FLOW,
    QTT_RESOURCE_ELABORATE_INVALID_RESOURCE_STATE,
} QttResourceElaborationError;

typedef struct {
    QttResourceError error;
    size_t allocated;
    size_t dropped;
    size_t moved_out;
    size_t projected_moves;
    size_t rehomes;
    size_t borrows;
    size_t retains;
    size_t releases;
    size_t live_owned;
    size_t peak_live_owned;
} QttHeapExecution;

QttRepresentation qtt_resource_classify_type(const Type *type);
QttResourceOp qtt_resource_alloc(QttCoreVar var);
QttResourceOp qtt_resource_alloc_typed(QttCoreVar var,
                                       QttRepresentation representation);
QttResourceOp qtt_resource_borrow(QttCoreVar var);
QttResourceOp qtt_resource_alias(QttCoreVar alias, QttCoreVar capability);
QttResourceOp qtt_resource_alias_exclusive(
    QttCoreVar alias, QttCoreVar capability);
QttResourceOp qtt_resource_alias_place(
    QttCoreVar alias, QttPlace place, QttLoanKind kind);
QttResourceOp qtt_resource_reborrow_place(
    QttCoreVar alias, QttCoreVar parent_alias,
    QttPlace place, QttLoanKind kind);
QttResourceOp qtt_resource_end_alias(
    QttCoreVar alias, QttCoreVar capability);
QttResourceOp qtt_resource_end_alias_exclusive(
    QttCoreVar alias, QttCoreVar capability);
QttResourceOp qtt_resource_end_alias_place(
    QttCoreVar alias, QttPlace place, QttLoanKind kind);
QttResourceOp qtt_resource_move(QttCoreVar var);
QttResourceOp qtt_resource_move_place(QttPlace place);
QttResourceOp qtt_resource_move_alias(
    QttCoreVar alias, QttCoreVar capability);
QttResourceOp qtt_resource_move_alias_place(
    QttCoreVar alias, QttPlace place);
QttResourceOp qtt_resource_write_alias(
    QttCoreVar alias, QttCoreVar capability);
QttResourceOp qtt_resource_write_alias_place(
    QttCoreVar alias, QttPlace place);
QttResourceOp qtt_resource_drop(QttCoreVar var);
QttResourceOp qtt_resource_dup(QttCoreVar source, QttCoreVar target);
/* Transfer one capability to a fresh local identity without retaining it. */
QttResourceOp qtt_resource_rehome(QttCoreVar source, QttCoreVar target);
QttResourceOp qtt_resource_replace(
    QttCoreVar var, QttRepresentation representation);
QttResourceOp qtt_resource_branch(QttResourceBlock *then_block,
                                  QttResourceBlock *else_block);
/*
 * Derive the canonical place-qualified effect witnessed by an access op.
 * Authority-only operations (including exclusive-loan acquisition) emit no
 * effect; mutation is witnessed only by WRITE_ALIAS.
 */
bool qtt_resource_op_effect_label(
    const QttResourceOp *op, char *buffer, size_t capacity);

QttResourceVerification qtt_resource_verify(
    const QttResourceBlock *block,
    const QttCoreVar *initially_owned,
    size_t initially_owned_count);
QttResourceVerification qtt_resource_verify_elaboration(
    const QttResourceBlock *block, const QttCoreNode *core);
QttResourceVerification qtt_resource_verify_elaboration_in_env(
    const QttResourceBlock *block, const QttCoreNode *core,
    const struct QttSignatureEnv *signatures, uint64_t module_id);
QttHeapExecution qtt_resource_execute(
    const QttResourceBlock *block,
    const bool *branch_choices,
    size_t branch_choice_count);

QttResourceBlock *qtt_resource_elaborate_lexical(
    const QttCoreNode *core,
    QttResourceElaborationError *error);
QttResourceBlock *qtt_resource_elaborate_lexical_in_env(
    const QttCoreNode *core,
    const struct QttSignatureEnv *signatures, uint64_t module_id,
    QttResourceElaborationError *error);
void qtt_resource_block_free(QttResourceBlock *block);

#endif
