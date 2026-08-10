#include "eval.h"

#include <stdlib.h>
#include <string.h>

typedef struct EvalEnv {
    QttCoreVar var;
    QttValue value;
    const struct EvalEnv *parent;
} EvalEnv;

typedef struct {
    bool *branches;
    size_t branch_count;
    size_t branch_capacity;
    QttEvalError error;
} EvalTrace;

static bool trace_branch(EvalTrace *trace, bool branch) {
    if (trace->branch_count == trace->branch_capacity) {
        size_t next = trace->branch_capacity ? trace->branch_capacity * 2 : 8;
        bool *grown = realloc(trace->branches, next * sizeof(*grown));
        if (!grown) {
            trace->error = QTT_EVAL_OUT_OF_MEMORY;
            return false;
        }
        trace->branches = grown;
        trace->branch_capacity = next;
    }
    trace->branches[trace->branch_count++] = branch;
    return true;
}

static bool env_lookup(const EvalEnv *env, QttCoreVar var, QttValue *value) {
    for (; env; env = env->parent) {
        if (!qtt_core_var_equal(env->var, var)) continue;
        *value = env->value;
        return true;
    }
    return false;
}

static QttValue eval_node(const QttCoreNode *node, const EvalEnv *env,
                          EvalTrace *trace) {
    QttValue unit = {.kind = QTT_VALUE_UNIT};
    if (!node || trace->error != QTT_EVAL_OK) return unit;
    switch (node->kind) {
    case QTT_CORE_LITERAL:
        if (!node->literal.source) {
            trace->error = QTT_EVAL_MALFORMED_LITERAL;
            return unit;
        }
        if (node->literal.source->type == AST_NUMBER)
            return (QttValue){
                .kind = QTT_VALUE_NUMBER,
                .number = node->literal.source->number,
            };
        if (node->literal.source->type == AST_STRING)
            return (QttValue){
                .kind = QTT_VALUE_STRING,
                .string = node->literal.source->string,
            };
        trace->error = QTT_EVAL_MALFORMED_LITERAL;
        return unit;
    case QTT_CORE_GLOBAL:
        if (node->global.name && strcmp(node->global.name, "True") == 0)
            return (QttValue){.kind = QTT_VALUE_BOOL, .boolean = true};
        if (node->global.name && strcmp(node->global.name, "False") == 0)
            return (QttValue){.kind = QTT_VALUE_BOOL, .boolean = false};
        trace->error = QTT_EVAL_UNKNOWN_GLOBAL;
        return unit;
    case QTT_CORE_VAR: {
        QttValue value;
        if (!env_lookup(env, node->var, &value)) {
            trace->error = QTT_EVAL_UNBOUND_VAR;
            return unit;
        }
        return value;
    }
    case QTT_CORE_BORROW:
        return eval_node(node->borrow.body, env, trace);
    case QTT_CORE_LET: {
        QttValue value = eval_node(node->let.value, env, trace);
        if (trace->error != QTT_EVAL_OK) return unit;
        EvalEnv binding = {
            .var = node->let.binding,
            .value = value,
            .parent = env,
        };
        return eval_node(node->let.body, &binding, trace);
    }
    case QTT_CORE_IF: {
        QttValue condition =
            eval_node(node->conditional.condition, env, trace);
        if (trace->error != QTT_EVAL_OK) return unit;
        if (condition.kind != QTT_VALUE_BOOL) {
            trace->error = QTT_EVAL_NON_BOOLEAN_CONDITION;
            return unit;
        }
        if (!trace_branch(trace, condition.boolean)) return unit;
        return eval_node(condition.boolean
                             ? node->conditional.then_branch
                             : node->conditional.else_branch,
                         env, trace);
    }
    case QTT_CORE_SEQUENCE: {
        QttValue value = unit;
        for (size_t i = 0; i < node->sequence.count; i++)
            value = eval_node(node->sequence.items[i], env, trace);
        return value;
    }
    default:
        trace->error = QTT_EVAL_UNSUPPORTED_CORE;
        return unit;
    }
}

QttEvalResult qtt_core_evaluate(const QttCoreNode *core) {
    EvalTrace trace = {.error = QTT_EVAL_OK};
    QttValue value = eval_node(core, NULL, &trace);
    free(trace.branches);
    return (QttEvalResult){.error = trace.error, .value = value};
}

QttCertifiedEvaluation qtt_resource_evaluate(const QttCoreNode *core) {
    QttCertifiedEvaluation result = {
        .eval_error = QTT_EVAL_OK,
        .elaboration_error = QTT_RESOURCE_ELABORATE_OK,
        .resource_error = QTT_RESOURCE_VALID,
    };
    EvalTrace trace = {.error = QTT_EVAL_OK};
    result.value = eval_node(core, NULL, &trace);
    result.eval_error = trace.error;
    result.branches = trace.branches;
    result.branch_count = trace.branch_count;
    if (trace.error != QTT_EVAL_OK) return result;
    QttResourceBlock *resource =
        qtt_resource_elaborate_lexical(core, &result.elaboration_error);
    if (!resource) {
        result.resource_error = QTT_RESOURCE_UNKNOWN_VAR;
        return result;
    }
    result.heap = qtt_resource_execute(resource, result.branches,
                                       result.branch_count);
    result.resource_error = result.heap.error;
    qtt_resource_block_free(resource);
    return result;
}

void qtt_certified_evaluation_free(QttCertifiedEvaluation *evaluation) {
    if (!evaluation) return;
    free(evaluation->branches);
    evaluation->branches = NULL;
    evaluation->branch_count = 0;
}
