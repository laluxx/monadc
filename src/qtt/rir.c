#include "rir.h"

#include <stdlib.h>
#include <string.h>

typedef struct RirEnv {
    QttCoreVar var;
    QttValue value;
    const struct RirEnv *parent;
} RirEnv;

typedef struct {
    bool *items;
    size_t count;
    size_t capacity;
    QttRirEvalError error;
} RirTrace;

static void free_node(QttRirNode *node);

static QttRirNode *lower_node(const QttCoreNode *core, QttRirError *error) {
    if (!core) {
        *error = QTT_RIR_UNSUPPORTED_CORE;
        return NULL;
    }
    QttRirNode *node = calloc(1, sizeof(*node));
    if (!node) {
        *error = QTT_RIR_OUT_OF_MEMORY;
        return NULL;
    }
    node->representation = qtt_resource_classify_type(core->type);
    switch (core->kind) {
    case QTT_CORE_LITERAL:
        node->kind = QTT_RIR_LITERAL;
        if (!core->literal.source) goto malformed;
        if (core->literal.source->type == AST_NUMBER)
            node->literal = (QttValue){
                .kind = QTT_VALUE_NUMBER,
                .number = core->literal.source->number,
            };
        else if (core->literal.source->type == AST_STRING)
            node->literal = (QttValue){
                .kind = QTT_VALUE_STRING,
                .string = core->literal.source->string,
            };
        else
            goto malformed;
        return node;
    case QTT_CORE_GLOBAL:
        node->kind = QTT_RIR_GLOBAL;
        node->global = core->global.name;
        return node;
    case QTT_CORE_VAR:
        node->kind = QTT_RIR_VAR;
        node->var = core->var;
        return node;
    case QTT_CORE_LET:
        node->kind = QTT_RIR_LET;
        node->let.binding = core->let.binding;
        node->let.value = lower_node(core->let.value, error);
        if (node->let.value)
            node->let.body = lower_node(core->let.body, error);
        if (!node->let.value || !node->let.body) goto failed;
        return node;
    case QTT_CORE_IF:
        node->kind = QTT_RIR_IF;
        node->conditional.condition =
            lower_node(core->conditional.condition, error);
        if (node->conditional.condition)
            node->conditional.then_branch =
                lower_node(core->conditional.then_branch, error);
        if (node->conditional.then_branch)
            node->conditional.else_branch =
                lower_node(core->conditional.else_branch, error);
        if (!node->conditional.condition ||
            !node->conditional.then_branch ||
            !node->conditional.else_branch)
            goto failed;
        return node;
    case QTT_CORE_SEQUENCE:
        node->kind = QTT_RIR_SEQUENCE;
        node->sequence.count = core->sequence.count;
        if (node->sequence.count)
            node->sequence.items =
                calloc(node->sequence.count, sizeof(*node->sequence.items));
        if (node->sequence.count && !node->sequence.items) {
            *error = QTT_RIR_OUT_OF_MEMORY;
            goto failed;
        }
        for (size_t i = 0; i < node->sequence.count; i++) {
            node->sequence.items[i] =
                lower_node(core->sequence.items[i], error);
            if (!node->sequence.items[i]) goto failed;
        }
        return node;
    default:
        *error = QTT_RIR_UNSUPPORTED_CORE;
        goto failed;
    }
malformed:
    *error = QTT_RIR_MALFORMED_LITERAL;
failed:
    free_node(node);
    return NULL;
}

static void free_node(QttRirNode *node) {
    if (!node) return;
    if (node->kind == QTT_RIR_LET) {
        free_node(node->let.value);
        free_node(node->let.body);
    } else if (node->kind == QTT_RIR_IF) {
        free_node(node->conditional.condition);
        free_node(node->conditional.then_branch);
        free_node(node->conditional.else_branch);
    } else if (node->kind == QTT_RIR_SEQUENCE) {
        for (size_t i = 0; i < node->sequence.count; i++)
            free_node(node->sequence.items[i]);
        free(node->sequence.items);
    }
    free(node);
}

QttRirProgram *qtt_rir_lower(const QttCoreNode *core, QttRirError *error) {
    QttRirError local = QTT_RIR_OK;
    QttRirProgram *program = calloc(1, sizeof(*program));
    if (!program) {
        if (error) *error = QTT_RIR_OUT_OF_MEMORY;
        return NULL;
    }
    program->root = lower_node(core, &local);
    if (!program->root) {
        free(program);
        if (error) *error = local;
        return NULL;
    }
    program->ownership =
        qtt_resource_elaborate_lexical(core, &program->ownership_error);
    if (error) *error = QTT_RIR_OK;
    return program;
}

static bool value_equal(QttValue left, QttValue right) {
    if (left.kind != right.kind) return false;
    if (left.kind == QTT_VALUE_NUMBER) return left.number == right.number;
    if (left.kind == QTT_VALUE_BOOL) return left.boolean == right.boolean;
    if (left.kind == QTT_VALUE_STRING)
        return left.string && right.string &&
               strcmp(left.string, right.string) == 0;
    return true;
}

bool qtt_rir_erases_to_core(const QttRirNode *rir, const QttCoreNode *core) {
    if (!rir || !core) return false;
    switch (rir->kind) {
    case QTT_RIR_LITERAL: {
        if (core->kind != QTT_CORE_LITERAL || !core->literal.source)
            return false;
        QttValue value;
        if (core->literal.source->type == AST_NUMBER)
            value = (QttValue){QTT_VALUE_NUMBER,
                               {.number = core->literal.source->number}};
        else if (core->literal.source->type == AST_STRING)
            value = (QttValue){QTT_VALUE_STRING,
                               {.string = core->literal.source->string}};
        else
            return false;
        return value_equal(rir->literal, value);
    }
    case QTT_RIR_GLOBAL:
        return core->kind == QTT_CORE_GLOBAL && rir->global &&
               core->global.name &&
               strcmp(rir->global, core->global.name) == 0;
    case QTT_RIR_VAR:
        return core->kind == QTT_CORE_VAR &&
               qtt_core_var_equal(rir->var, core->var);
    case QTT_RIR_LET:
        return core->kind == QTT_CORE_LET &&
               qtt_core_var_equal(rir->let.binding, core->let.binding) &&
               qtt_rir_erases_to_core(rir->let.value, core->let.value) &&
               qtt_rir_erases_to_core(rir->let.body, core->let.body);
    case QTT_RIR_IF:
        return core->kind == QTT_CORE_IF &&
               qtt_rir_erases_to_core(rir->conditional.condition,
                                      core->conditional.condition) &&
               qtt_rir_erases_to_core(rir->conditional.then_branch,
                                      core->conditional.then_branch) &&
               qtt_rir_erases_to_core(rir->conditional.else_branch,
                                      core->conditional.else_branch);
    case QTT_RIR_SEQUENCE:
        if (core->kind != QTT_CORE_SEQUENCE ||
            rir->sequence.count != core->sequence.count)
            return false;
        for (size_t i = 0; i < rir->sequence.count; i++)
            if (!qtt_rir_erases_to_core(rir->sequence.items[i],
                                        core->sequence.items[i]))
                return false;
        return true;
    }
    return false;
}

static bool lookup(const RirEnv *env, QttCoreVar var, QttValue *value) {
    for (; env; env = env->parent)
        if (qtt_core_var_equal(env->var, var)) {
            *value = env->value;
            return true;
        }
    return false;
}

static bool trace_push(RirTrace *trace, bool branch) {
    if (trace->count == trace->capacity) {
        size_t next = trace->capacity ? trace->capacity * 2 : 8;
        bool *grown = realloc(trace->items, next * sizeof(*grown));
        if (!grown) {
            trace->error = QTT_RIR_EVAL_OUT_OF_MEMORY;
            return false;
        }
        trace->items = grown;
        trace->capacity = next;
    }
    trace->items[trace->count++] = branch;
    return true;
}

static QttValue evaluate(const QttRirNode *node, const RirEnv *env,
                         RirTrace *trace) {
    QttValue unit = {.kind = QTT_VALUE_UNIT};
    if (!node || trace->error != QTT_RIR_EVAL_OK) return unit;
    switch (node->kind) {
    case QTT_RIR_LITERAL:
        return node->literal;
    case QTT_RIR_GLOBAL:
        if (node->global && strcmp(node->global, "True") == 0)
            return (QttValue){.kind = QTT_VALUE_BOOL, .boolean = true};
        if (node->global && strcmp(node->global, "False") == 0)
            return (QttValue){.kind = QTT_VALUE_BOOL, .boolean = false};
        trace->error = QTT_RIR_EVAL_UNKNOWN_GLOBAL;
        return unit;
    case QTT_RIR_VAR: {
        QttValue value;
        if (!lookup(env, node->var, &value)) {
            trace->error = QTT_RIR_EVAL_UNBOUND_VAR;
            return unit;
        }
        return value;
    }
    case QTT_RIR_LET: {
        QttValue value = evaluate(node->let.value, env, trace);
        RirEnv binding = {node->let.binding, value, env};
        return evaluate(node->let.body, &binding, trace);
    }
    case QTT_RIR_IF: {
        QttValue condition =
            evaluate(node->conditional.condition, env, trace);
        if (trace->error != QTT_RIR_EVAL_OK) return unit;
        if (condition.kind != QTT_VALUE_BOOL) {
            trace->error = QTT_RIR_EVAL_NON_BOOLEAN_CONDITION;
            return unit;
        }
        if (!trace_push(trace, condition.boolean)) return unit;
        return evaluate(condition.boolean
                            ? node->conditional.then_branch
                            : node->conditional.else_branch,
                        env, trace);
    }
    case QTT_RIR_SEQUENCE: {
        QttValue value = unit;
        for (size_t i = 0; i < node->sequence.count; i++)
            value = evaluate(node->sequence.items[i], env, trace);
        return value;
    }
    }
    return unit;
}

QttRirEvaluation qtt_rir_evaluate(const QttRirProgram *program) {
    QttRirEvaluation result = {.error = QTT_RIR_EVAL_OK};
    if (!program || !program->root) {
        result.error = QTT_RIR_EVAL_INVALID_OWNERSHIP;
        return result;
    }
    RirTrace trace = {.error = QTT_RIR_EVAL_OK};
    result.value = evaluate(program->root, NULL, &trace);
    result.error = trace.error;
    result.branches = trace.items;
    result.branch_count = trace.count;
    if (result.error != QTT_RIR_EVAL_OK) return result;
    if (!program->ownership) {
        result.error = QTT_RIR_EVAL_INVALID_OWNERSHIP;
        return result;
    }
    result.heap = qtt_resource_execute(program->ownership, result.branches,
                                       result.branch_count);
    if (result.heap.error != QTT_RESOURCE_VALID)
        result.error = QTT_RIR_EVAL_INVALID_OWNERSHIP;
    return result;
}

void qtt_rir_evaluation_free(QttRirEvaluation *evaluation) {
    if (!evaluation) return;
    free(evaluation->branches);
    evaluation->branches = NULL;
    evaluation->branch_count = 0;
}

void qtt_rir_program_free(QttRirProgram *program) {
    if (!program) return;
    free_node(program->root);
    qtt_resource_block_free(program->ownership);
    free(program);
}
