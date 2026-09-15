#ifndef MONAD_QTT_RIR_H
#define MONAD_QTT_RIR_H

#include "eval.h"

typedef enum {
    QTT_RIR_LITERAL,
    QTT_RIR_GLOBAL,
    QTT_RIR_VAR,
    QTT_RIR_LET,
    QTT_RIR_IF,
    QTT_RIR_SEQUENCE,
} QttRirKind;

typedef struct QttRirNode QttRirNode;

struct QttRirNode {
    QttRirKind kind;
    QttRepresentation representation;
    union {
        QttValue literal;
        const char *global;
        QttCoreVar var;
        struct {
            QttCoreVar binding;
            QttRirNode *value;
            QttRirNode *body;
        } let;
        struct {
            QttRirNode *condition;
            QttRirNode *then_branch;
            QttRirNode *else_branch;
        } conditional;
        struct {
            QttRirNode **items;
            size_t count;
        } sequence;
    };
};

typedef enum {
    QTT_RIR_OK,
    QTT_RIR_OUT_OF_MEMORY,
    QTT_RIR_UNSUPPORTED_CORE,
    QTT_RIR_MALFORMED_LITERAL,
} QttRirError;

typedef struct {
    QttRirNode *root;
    QttResourceBlock *ownership;
    QttResourceElaborationError ownership_error;
} QttRirProgram;

typedef enum {
    QTT_RIR_EVAL_OK,
    QTT_RIR_EVAL_OUT_OF_MEMORY,
    QTT_RIR_EVAL_UNBOUND_VAR,
    QTT_RIR_EVAL_UNKNOWN_GLOBAL,
    QTT_RIR_EVAL_NON_BOOLEAN_CONDITION,
    QTT_RIR_EVAL_INVALID_OWNERSHIP,
} QttRirEvalError;

typedef struct {
    QttRirEvalError error;
    QttValue value;
    bool *branches;
    size_t branch_count;
    QttHeapExecution heap;
} QttRirEvaluation;

QttRirProgram *qtt_rir_lower(const QttCoreNode *core, QttRirError *error);
bool qtt_rir_erases_to_core(const QttRirNode *rir, const QttCoreNode *core);
QttRirEvaluation qtt_rir_evaluate(const QttRirProgram *program);
void qtt_rir_evaluation_free(QttRirEvaluation *evaluation);
void qtt_rir_program_free(QttRirProgram *program);

#endif
