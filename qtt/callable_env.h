#ifndef MONAD_QTT_CALLABLE_ENV_H
#define MONAD_QTT_CALLABLE_ENV_H

#include "core.h"

#include <stdlib.h>

/* Immutable lexical evidence that a BinderId denotes one exact code value. */
typedef struct QttCallableBinding {
    QttCoreVar var;
    const QttCoreNode *lambda;
    const struct QttCallableBinding *parent;
} QttCallableBinding;

static inline const QttCoreNode *qtt_callable_env_lookup(
    const QttCallableBinding *environment, QttCoreVar var) {
    for (; environment; environment = environment->parent)
        if (qtt_core_var_equal(environment->var, var))
            return environment->lambda;
    return NULL;
}

static inline bool qtt_callable_capture_free(
    const QttCoreNode *node) {
    if (!node || node->kind != QTT_CORE_LAMBDA) return false;
    QttCoreVar *captures = NULL;
    size_t count = 0;
    QttCoreError error = qtt_core_lambda_captures(
        node, &captures, &count);
    free(captures);
    return error == QTT_CORE_OK && count == 0;
}

static inline bool qtt_callable_exact_lambda(
    const QttCoreNode *node) {
    if (!node || node->kind != QTT_CORE_LAMBDA) return false;
    QttCoreVar *captures = NULL;
    size_t count = 0;
    QttCoreError error = qtt_core_lambda_captures(
        node, &captures, &count);
    free(captures);
    return error == QTT_CORE_OK;
}

static inline const QttCoreNode *qtt_callable_env_value(
    const QttCallableBinding *environment,
    const QttCoreNode *value) {
    if (qtt_callable_exact_lambda(value)) return value;
    return value && value->kind == QTT_CORE_VAR
        ? qtt_callable_env_lookup(environment, value->var)
        : NULL;
}

#endif
