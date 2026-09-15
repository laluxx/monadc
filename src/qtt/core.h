#ifndef MONAD_QTT_CORE_H
#define MONAD_QTT_CORE_H

#include "../reader.h"
#include "../types.h"
#include "quantity.h"

/*
 * Typed Core is the semantic boundary between source elaboration and resource
 * inference. It makes binding identity, evaluation order, and control joins
 * explicit while borrowing zonked Type objects from the checked AST.
 *
 * The separation of a typed term calculus from quantitative annotations
 * follows Atkey's QTT presentation:
 * https://bentnib.org/quantitative-type-theory.pdf
 */

typedef struct {
    uint64_t module_id;
    uint64_t binder_id;
} QttCoreVar;

typedef struct {
    QttCoreVar root;
    /* Legacy one-level identity; zero denotes the whole root. */
    uint64_t projection_id;
    /* Certified prefix path, with one-based structural field ordinals. */
#define QTT_PLACE_MAX_DEPTH 8
    uint32_t projection_path[QTT_PLACE_MAX_DEPTH];
    uint8_t projection_depth;
} QttPlace;

/* Loan polarity belongs to typed Core: later IRs preserve this witness. */
typedef enum {
    QTT_LOAN_SHARED,
    QTT_LOAN_EXCLUSIVE,
} QttLoanKind;

typedef enum {
    QTT_CORE_RESUMPTION_ABORTIVE,
    QTT_CORE_RESUMPTION_ONE_SHOT,
    QTT_CORE_RESUMPTION_MULTI_SHOT,
    QTT_CORE_RESUMPTION_UNRESTRICTED,
} QttCoreResumptionClass;

typedef enum {
    QTT_CORE_LITERAL,
    QTT_CORE_GLOBAL,
    QTT_CORE_VAR,
    QTT_CORE_LAMBDA,
    QTT_CORE_APPLY,
    QTT_CORE_LET,
    QTT_CORE_IF,
    QTT_CORE_SEQUENCE,
    /* A type-certified structural place read; tail use may evacuate it. */
    QTT_CORE_PLACE,
    QTT_CORE_WRITE,
    /* Lexically scoped loan; parent={0,0} denotes a root borrow. */
    QTT_CORE_BORROW,
    /* Algebraic operation request with a stable, module-scoped capability. */
    QTT_CORE_PERFORM,
    /* Profile-directed elimination boundary; the clause receives resumption. */
    QTT_CORE_HANDLE,
    QTT_CORE_QUOTE,
} QttCoreKind;

typedef enum {
    QTT_CORE_OK,
    QTT_CORE_OUT_OF_MEMORY,
    QTT_CORE_UNSUPPORTED_AST,
    QTT_CORE_MALFORMED_AST,
} QttCoreError;

typedef enum {
    QTT_CORE_VALID,
    QTT_CORE_UNBOUND_VAR,
    QTT_CORE_WRONG_MODULE,
    QTT_CORE_DUPLICATE_BINDER,
    QTT_CORE_VALIDATION_OUT_OF_MEMORY,
} QttCoreValidation;

typedef struct QttCoreNode QttCoreNode;

struct QttCoreNode {
    QttCoreKind kind;
    const Type *type;
    /* Borrowed checked source node, retained for proof-directed lowering. */
    const AST *source;
    int line;
    int column;
    union {
        QttCoreVar var;
        struct {
            const char *name;
            /* Owned snapshot of canonical compiler-module authority. */
            char *callable_contract;
            uint64_t callable_contract_fingerprint;
            char *hm_scheme;
            size_t *effect_predicate_stages;
            char **effect_predicate_names;
            size_t effect_predicate_count;
            uint64_t effect_row_fingerprint;
            uint64_t effect_constraint_fingerprint;
            int effect_constraint_result;
            bool authority_owned;
        } global;
        struct {
            QttCoreVar *params;
            const Type **param_types;
            size_t param_count;
            QttCoreNode *body;
        } lambda;
        struct {
            QttCoreNode *callee;
            QttCoreNode **arguments;
            size_t argument_count;
        } apply;
        struct {
            QttCoreVar binding;
            QttCoreNode *value;
            QttCoreNode *body;
        } let;
        struct {
            QttCoreNode *condition;
            QttCoreNode *then_branch;
            QttCoreNode *else_branch;
        } conditional;
        struct {
            QttCoreNode **items;
            size_t count;
        } sequence;
        struct {
            QttPlace place;
            QttCoreNode *value;
        } write;
        struct {
            QttCoreVar binding;
            QttCoreVar parent;
            QttPlace place;
            QttLoanKind loan_kind;
            QttCoreNode *body;
        } borrow;
        struct {
            const char *effect_name;
            uint64_t capability_id;
            uint64_t constructor_id;
            QttQuantity resumption;
            bool scoped;
            QttCoreNode *argument;
        } perform;
        struct {
            const char *profile_name;
            uint64_t capability_id;
            QttCoreResumptionClass resumption_class;
            bool deep;
            char *portable_proof;
            /* Non-owning unique operation selected by effect elaboration. */
            QttCoreNode *selected_operation;
            /* True only when the selected operation is enclosed by a strict,
             * structurally pure context admissible for direct q=0 erasure. */
            bool abortive_context_safe;
            QttCoreNode *computation;
            QttCoreNode *clause;
        } handle;
        QttPlace place;
        struct {
            const AST *source;
        } literal;
    };
};

bool qtt_core_var_equal(QttCoreVar left, QttCoreVar right);
QttCoreNode *qtt_core_lower(const AST *ast, uint64_t module_id,
                            QttCoreError *error);
QttCoreValidation qtt_core_validate(const QttCoreNode *node,
                                    uint64_t module_id);
QttCoreError qtt_core_lambda_captures(const QttCoreNode *lambda,
                                      QttCoreVar **captures,
                                      size_t *capture_count);
const Type *qtt_core_var_type(
    const QttCoreNode *node, QttCoreVar var);
void qtt_core_free(QttCoreNode *node);

#endif
