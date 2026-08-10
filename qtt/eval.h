#ifndef MONAD_QTT_EVAL_H
#define MONAD_QTT_EVAL_H

#include "resource.h"

typedef enum {
    QTT_VALUE_UNIT,
    QTT_VALUE_NUMBER,
    QTT_VALUE_BOOL,
    QTT_VALUE_STRING,
} QttValueKind;

typedef struct {
    QttValueKind kind;
    union {
        double number;
        bool boolean;
        const char *string;
    };
} QttValue;

typedef enum {
    QTT_EVAL_OK,
    QTT_EVAL_OUT_OF_MEMORY,
    QTT_EVAL_UNBOUND_VAR,
    QTT_EVAL_UNKNOWN_GLOBAL,
    QTT_EVAL_NON_BOOLEAN_CONDITION,
    QTT_EVAL_UNSUPPORTED_CORE,
    QTT_EVAL_MALFORMED_LITERAL,
} QttEvalError;

typedef struct {
    QttEvalError error;
    QttValue value;
} QttEvalResult;

typedef struct {
    QttEvalError eval_error;
    QttResourceElaborationError elaboration_error;
    QttResourceError resource_error;
    QttValue value;
    bool *branches;
    size_t branch_count;
    QttHeapExecution heap;
} QttCertifiedEvaluation;

QttEvalResult qtt_core_evaluate(const QttCoreNode *core);
QttCertifiedEvaluation qtt_resource_evaluate(const QttCoreNode *core);
void qtt_certified_evaluation_free(QttCertifiedEvaluation *evaluation);

#endif
