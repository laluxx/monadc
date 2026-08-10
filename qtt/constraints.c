#include "constraints.h"

#include <stdlib.h>
#include <string.h>

struct QttGradeExpr {
    QttGradeExprKind kind;
    QttQuantity constant;
    uint64_t variable;
    struct QttGradeExpr *left;
    struct QttGradeExpr *right;
};

struct QttGradeArena {
    QttGradeExpr **nodes;
    size_t count;
    size_t capacity;
    uint64_t next_variable;
    size_t references;
};

typedef struct {
    QttGradeExpr *left;
    QttGradeExpr *right;
    bool equality;
} QttGradeConstraint;

struct QttGradeSolver {
    QttGradeArena *arena;
    QttGradeConstraint *constraints;
    size_t count;
    size_t capacity;
    QttQuantity *solutions;
    size_t solution_count;
};

struct QttGradeScheme {
    QttGradeArena *arena;
    size_t references;
    QttGradeExpr **expressions;
    size_t expression_count;
    size_t domain_count;
    uint64_t *closure_module_ids;
    uint64_t *closure_ids;
    uint64_t *closure_binder_ids;
    size_t *closure_slots;
    size_t *closure_parameter_indices;
    QttEnvironmentOriginKind *closure_origin_kinds;
    uint64_t *closure_origin_ids;
    size_t closure_count;
    uint64_t *closure_domain_module_ids;
    uint64_t *closure_domain_ids;
    size_t *closure_domain_indices;
    size_t closure_domain_count;
    uint64_t *result_closure_module_ids;
    uint64_t *result_closure_ids;
    size_t result_closure_count;
    size_t *callable_parameter_indices;
    size_t callable_parameter_count;
    size_t *callable_domain_parameter_indices;
    size_t *callable_domain_indices;
    size_t callable_domain_count;
    uint64_t *quantified;
    size_t quantified_count;
};

static QttGradeExpr *arena_node(QttGradeArena *arena) {
    if (!arena) return NULL;
    if (arena->count == arena->capacity) {
        size_t next = arena->capacity ? arena->capacity * 2 : 32;
        QttGradeExpr **grown =
            realloc(arena->nodes, next * sizeof(*grown));
        if (!grown) return NULL;
        arena->nodes = grown;
        arena->capacity = next;
    }
    QttGradeExpr *node = calloc(1, sizeof(*node));
    if (!node) return NULL;
    arena->nodes[arena->count++] = node;
    return node;
}

QttGradeArena *qtt_grade_arena_new(void) {
    QttGradeArena *arena = calloc(1, sizeof(*arena));
    if (arena) {
        arena->next_variable = 1;
        arena->references = 1;
    }
    return arena;
}

void qtt_grade_arena_free(QttGradeArena *arena) {
    if (!arena) return;
    if (--arena->references) return;
    for (size_t i = 0; i < arena->count; i++) free(arena->nodes[i]);
    free(arena->nodes);
    free(arena);
}

static QttGradeArena *grade_arena_retain(QttGradeArena *arena) {
    if (arena) arena->references++;
    return arena;
}

QttGradeExpr *qtt_grade_constant(
    QttGradeArena *arena, QttQuantity quantity) {
    QttGradeExpr *node = arena_node(arena);
    if (node) {
        node->kind = QTT_GRADE_CONSTANT;
        node->constant = quantity;
    }
    return node;
}

QttGradeExpr *qtt_grade_fresh(QttGradeArena *arena) {
    QttGradeExpr *node = arena_node(arena);
    if (node) {
        node->kind = QTT_GRADE_VARIABLE;
        node->variable = arena->next_variable++;
    }
    return node;
}

static QttGradeExpr *binary(
    QttGradeArena *arena, QttGradeExprKind kind,
    QttGradeExpr *left, QttGradeExpr *right) {
    if (!left || !right) return NULL;
    QttGradeExpr *node = arena_node(arena);
    if (node) {
        node->kind = kind;
        node->left = left;
        node->right = right;
    }
    return node;
}

QttGradeExpr *qtt_grade_add(
    QttGradeArena *arena, QttGradeExpr *left, QttGradeExpr *right) {
    return binary(arena, QTT_GRADE_ADD, left, right);
}

QttGradeExpr *qtt_grade_multiply(
    QttGradeArena *arena, QttGradeExpr *left, QttGradeExpr *right) {
    return binary(arena, QTT_GRADE_MULTIPLY, left, right);
}

QttGradeExpr *qtt_grade_maximum(
    QttGradeArena *arena, QttGradeExpr *left, QttGradeExpr *right) {
    return binary(arena, QTT_GRADE_MAXIMUM, left, right);
}

QttGradeExpr *qtt_grade_substitute(
    QttGradeArena *arena, const QttGradeExpr *expression,
    const QttGradeExpr *variable, QttGradeExpr *replacement) {
    if (!arena || !expression || !variable || !replacement ||
        variable->kind != QTT_GRADE_VARIABLE)
        return NULL;
    if (expression->kind == QTT_GRADE_VARIABLE)
        return expression->variable == variable->variable
            ? replacement : (QttGradeExpr *)expression;
    if (expression->kind == QTT_GRADE_CONSTANT)
        return (QttGradeExpr *)expression;
    QttGradeExpr *left = qtt_grade_substitute(
        arena, expression->left, variable, replacement);
    QttGradeExpr *right = qtt_grade_substitute(
        arena, expression->right, variable, replacement);
    if (!left || !right) return NULL;
    if (expression->kind == QTT_GRADE_ADD)
        return qtt_grade_add(arena, left, right);
    if (expression->kind == QTT_GRADE_MULTIPLY)
        return qtt_grade_multiply(arena, left, right);
    return qtt_grade_maximum(arena, left, right);
}

QttGradeExprKind qtt_grade_expr_kind(const QttGradeExpr *expression) {
    return expression ? expression->kind : QTT_GRADE_CONSTANT;
}

uint64_t qtt_grade_variable_id(const QttGradeExpr *expression) {
    return expression && expression->kind == QTT_GRADE_VARIABLE
        ? expression->variable : 0;
}

QttQuantity qtt_grade_constant_value(const QttGradeExpr *expression) {
    return expression && expression->kind == QTT_GRADE_CONSTANT
        ? expression->constant : qtt_quantity_finite(0);
}

QttGradeSolver *qtt_grade_solver_new(QttGradeArena *arena) {
    if (!arena) return NULL;
    QttGradeSolver *solver = calloc(1, sizeof(*solver));
    if (solver) solver->arena = arena;
    return solver;
}

void qtt_grade_solver_free(QttGradeSolver *solver) {
    if (!solver) return;
    free(solver->constraints);
    free(solver->solutions);
    free(solver);
}

static bool constrain(
    QttGradeSolver *solver, QttGradeExpr *left,
    QttGradeExpr *right, bool equality) {
    if (!solver || !left || !right) return false;
    if (solver->count == solver->capacity) {
        size_t next = solver->capacity ? solver->capacity * 2 : 16;
        QttGradeConstraint *grown = realloc(
            solver->constraints, next * sizeof(*grown));
        if (!grown) return false;
        solver->constraints = grown;
        solver->capacity = next;
    }
    solver->constraints[solver->count++] =
        (QttGradeConstraint){left, right, equality};
    return true;
}

bool qtt_grade_constrain_leq(
    QttGradeSolver *solver, QttGradeExpr *left, QttGradeExpr *right) {
    return constrain(solver, left, right, false);
}

bool qtt_grade_constrain_equal(
    QttGradeSolver *solver, QttGradeExpr *left, QttGradeExpr *right) {
    return constrain(solver, left, right, true);
}

static bool occurs(uint64_t variable, const QttGradeExpr *expression) {
    if (!expression) return false;
    if (expression->kind == QTT_GRADE_VARIABLE)
        return expression->variable == variable;
    if (expression->kind == QTT_GRADE_ADD ||
        expression->kind == QTT_GRADE_MULTIPLY ||
        expression->kind == QTT_GRADE_MAXIMUM)
        return occurs(variable, expression->left) ||
               occurs(variable, expression->right);
    return false;
}

static QttQuantity quantity_maximum(
    QttQuantity left, QttQuantity right);

static QttQuantity evaluate(
    const QttGradeExpr *expression,
    const QttQuantity *solutions, size_t count) {
    if (expression->kind == QTT_GRADE_CONSTANT)
        return expression->constant;
    if (expression->kind == QTT_GRADE_VARIABLE) {
        size_t index = (size_t)expression->variable;
        return index < count ? solutions[index]
                             : qtt_quantity_finite(0);
    }
    QttQuantity left = evaluate(expression->left, solutions, count);
    QttQuantity right = evaluate(expression->right, solutions, count);
    if (expression->kind == QTT_GRADE_ADD)
        return qtt_quantity_add(left, right);
    if (expression->kind == QTT_GRADE_MULTIPLY)
        return qtt_quantity_multiply(left, right);
    return quantity_maximum(left, right);
}

static QttQuantity quantity_maximum(
    QttQuantity left, QttQuantity right) {
    return qtt_quantity_leq(left, right) ? right : left;
}

QttGradeSolveResult qtt_grade_solve(QttGradeSolver *solver) {
    if (!solver) return QTT_GRADE_UNSUPPORTED_CONSTRAINT;
    size_t variables = (size_t)solver->arena->next_variable;
    QttQuantity *solutions =
        calloc(variables ? variables : 1, sizeof(*solutions));
    QttQuantity *uppers =
        malloc((variables ? variables : 1) * sizeof(*uppers));
    if (!solutions || !uppers) {
        free(solutions);
        free(uppers);
        return QTT_GRADE_SOLVER_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < variables; i++)
        uppers[i] = qtt_quantity_omega();
    for (size_t i = 0; i < solver->count; i++) {
        QttGradeConstraint *constraint = &solver->constraints[i];
        if (constraint->equality &&
            constraint->left->kind == QTT_GRADE_VARIABLE &&
            occurs(constraint->left->variable, constraint->right) &&
            constraint->right != constraint->left) {
            free(solutions); free(uppers);
            return QTT_GRADE_OCCURS_CYCLE;
        }
        if (constraint->equality &&
            constraint->right->kind == QTT_GRADE_VARIABLE &&
            occurs(constraint->right->variable, constraint->left) &&
            constraint->left != constraint->right) {
            free(solutions); free(uppers);
            return QTT_GRADE_OCCURS_CYCLE;
        }
    }
    bool changed;
    size_t iterations = 0;
    do {
        changed = false;
        for (size_t i = 0; i < solver->count; i++) {
            QttGradeConstraint *constraint = &solver->constraints[i];
            QttQuantity left =
                evaluate(constraint->left, solutions, variables);
            QttQuantity right =
                evaluate(constraint->right, solutions, variables);
            if (constraint->right->kind == QTT_GRADE_VARIABLE) {
                size_t variable =
                    (size_t)constraint->right->variable;
                QttQuantity next =
                    quantity_maximum(solutions[variable], left);
                if (!qtt_quantity_equal(next, solutions[variable])) {
                    solutions[variable] = next;
                    changed = true;
                }
            }
            if (constraint->left->kind == QTT_GRADE_VARIABLE &&
                constraint->right->kind == QTT_GRADE_CONSTANT) {
                size_t variable =
                    (size_t)constraint->left->variable;
                uppers[variable] = constraint->equality
                    ? right : quantity_maximum(
                        qtt_quantity_finite(0), right);
                if (constraint->equality &&
                    !qtt_quantity_equal(solutions[variable], right)) {
                    solutions[variable] = right;
                    changed = true;
                }
            }
            if (constraint->equality &&
                constraint->left->kind == QTT_GRADE_VARIABLE &&
                constraint->right->kind != QTT_GRADE_VARIABLE) {
                size_t variable =
                    (size_t)constraint->left->variable;
                if (!qtt_quantity_equal(solutions[variable], right)) {
                    solutions[variable] = right;
                    changed = true;
                }
            }
        }
    } while (changed && ++iterations <= variables + solver->count + 1);
    for (size_t i = 0; i < solver->count; i++) {
        QttGradeConstraint *constraint = &solver->constraints[i];
        QttQuantity left =
            evaluate(constraint->left, solutions, variables);
        QttQuantity right =
            evaluate(constraint->right, solutions, variables);
        if ((!constraint->equality && !qtt_quantity_leq(left, right)) ||
            (constraint->equality && !qtt_quantity_equal(left, right))) {
            free(solutions); free(uppers);
            return QTT_GRADE_UNSATISFIABLE;
        }
    }
    for (size_t i = 1; i < variables; i++)
        if (!qtt_quantity_leq(solutions[i], uppers[i])) {
            free(solutions); free(uppers);
            return QTT_GRADE_UNSATISFIABLE;
        }
    free(uppers);
    free(solver->solutions);
    solver->solutions = solutions;
    solver->solution_count = variables;
    return QTT_GRADE_SOLVED;
}

QttQuantity qtt_grade_solution(
    const QttGradeSolver *solver, const QttGradeExpr *expression) {
    if (!solver || !solver->solutions || !expression)
        return qtt_quantity_omega();
    return evaluate(expression, solver->solutions, solver->solution_count);
}

static bool id_present(const uint64_t *ids, size_t count, uint64_t id) {
    for (size_t i = 0; i < count; i++) if (ids[i] == id) return true;
    return false;
}

static bool collect_variables(
    const QttGradeExpr *expression, uint64_t **ids,
    size_t *count, size_t *capacity) {
    if (expression->kind == QTT_GRADE_VARIABLE) {
        if (id_present(*ids, *count, expression->variable)) return true;
        if (*count == *capacity) {
            size_t next = *capacity ? *capacity * 2 : 8;
            uint64_t *grown = realloc(*ids, next * sizeof(*grown));
            if (!grown) return false;
            *ids = grown; *capacity = next;
        }
        (*ids)[(*count)++] = expression->variable;
        return true;
    }
    if (expression->kind == QTT_GRADE_ADD ||
        expression->kind == QTT_GRADE_MULTIPLY ||
        expression->kind == QTT_GRADE_MAXIMUM)
        return collect_variables(
                   expression->left, ids, count, capacity) &&
               collect_variables(
                   expression->right, ids, count, capacity);
    return true;
}

QttGradeScheme *qtt_grade_generalize(
    QttGradeArena *arena, QttGradeExpr *const *expressions, size_t count) {
    QttGradeScheme *scheme = qtt_grade_generalize_excluding(
        arena, expressions, count, NULL, 0);
    if (scheme) scheme->domain_count = count;
    return scheme;
}

QttGradeScheme *qtt_grade_generalize_signature(
    QttGradeArena *arena, const QttGradeSignature *signature) {
    if (!signature) return NULL;
    size_t domain_count = signature->domain_count;
    size_t closure_count = signature->closure_count;
    size_t closure_domain_count = signature->closure_domain_count;
    size_t result_count = signature->result_closure_count;
    size_t callable_count = signature->callable_parameter_count;
    size_t callable_domain_count = signature->callable_domain_count;
    if ((domain_count && !signature->domain_expressions) ||
        (closure_count &&
         (!signature->closure_module_ids || !signature->closure_ids ||
          !signature->closure_binder_ids || !signature->closure_slots ||
          !signature->closure_parameter_indices ||
          !signature->closure_expressions)) ||
        (closure_domain_count &&
         (!signature->closure_domain_module_ids ||
          !signature->closure_domain_ids ||
          !signature->closure_domain_indices ||
          !signature->closure_domain_expressions)) ||
        (result_count &&
         (!signature->result_closure_module_ids ||
          !signature->result_closure_ids)) ||
        (callable_count &&
         (!signature->callable_parameter_indices ||
          !signature->callable_invocation_expressions)) ||
        (callable_domain_count &&
         (!signature->callable_domain_parameter_indices ||
          !signature->callable_domain_indices ||
          !signature->callable_domain_expressions)))
        return NULL;
    for (size_t i = 0; i < callable_count; i++) {
        if (signature->callable_parameter_indices[i] >= domain_count)
            return NULL;
        if (!signature->callable_invocation_expressions[i])
            return NULL;
        for (size_t j = 0; j < i; j++)
            if (signature->callable_parameter_indices[j] ==
                signature->callable_parameter_indices[i])
                return NULL;
    }
    for (size_t i = 0; i < callable_domain_count; i++) {
        size_t parameter =
            signature->callable_domain_parameter_indices[i];
        if (parameter >= domain_count ||
            !signature->callable_domain_expressions[i])
            return NULL;
        bool callable = false;
        for (size_t j = 0; j < callable_count; j++)
            if (signature->callable_parameter_indices[j] == parameter) {
                callable = true;
                break;
            }
        if (!callable) return NULL;
        for (size_t j = 0; j < i; j++)
            if (signature->callable_domain_parameter_indices[j] ==
                    parameter &&
                signature->callable_domain_indices[j] ==
                    signature->callable_domain_indices[i])
                return NULL;
    }
    for (size_t i = 0; i < closure_domain_count; i++) {
        if (!signature->closure_domain_module_ids[i] ||
            !signature->closure_domain_ids[i] ||
            !signature->closure_domain_expressions[i])
            return NULL;
        size_t qualified_count = 0;
        for (size_t j = 0; j < closure_domain_count; j++)
            if (signature->closure_domain_module_ids[j] ==
                    signature->closure_domain_module_ids[i] &&
                signature->closure_domain_ids[j] ==
                    signature->closure_domain_ids[i])
                qualified_count++;
        if (signature->closure_domain_indices[i] >= qualified_count)
            return NULL;
        for (size_t j = 0; j < i; j++)
            if (signature->closure_domain_module_ids[j] ==
                    signature->closure_domain_module_ids[i] &&
                signature->closure_domain_ids[j] ==
                    signature->closure_domain_ids[i] &&
                signature->closure_domain_indices[j] ==
                    signature->closure_domain_indices[i])
                return NULL;
    }
    for (size_t i = 0; i < closure_count; i++) {
        if (!signature->closure_module_ids[i] ||
            !signature->closure_ids[i] ||
            !signature->closure_binder_ids[i] ||
            !signature->closure_expressions[i])
            return NULL;
        if (signature->closure_parameter_indices[i] != SIZE_MAX &&
            signature->closure_parameter_indices[i] >= domain_count)
            return NULL;
        if ((signature->closure_origin_kinds &&
             !signature->closure_origin_ids) ||
            (!signature->closure_origin_kinds &&
             signature->closure_origin_ids))
            return NULL;
        if (signature->closure_origin_kinds) {
            QttEnvironmentOriginKind kind =
                signature->closure_origin_kinds[i];
            uint64_t identity = signature->closure_origin_ids[i];
            if ((kind != QTT_ENVIRONMENT_PARAMETER &&
                 kind != QTT_ENVIRONMENT_LOCAL &&
                 kind != QTT_ENVIRONMENT_EXPRESSION) ||
                (kind == QTT_ENVIRONMENT_PARAMETER
                     ? identity >= domain_count
                     : identity == 0))
                return NULL;
        }
        size_t qualified_count = 0;
        for (size_t j = 0; j < closure_count; j++)
            if (signature->closure_module_ids[j] ==
                    signature->closure_module_ids[i] &&
                signature->closure_ids[j] == signature->closure_ids[i])
                qualified_count++;
        if (signature->closure_slots[i] >= qualified_count)
            return NULL;
        for (size_t j = 0; j < i; j++)
            if (signature->closure_module_ids[j] ==
                    signature->closure_module_ids[i] &&
                signature->closure_ids[j] == signature->closure_ids[i] &&
                signature->closure_slots[j] ==
                    signature->closure_slots[i])
                return NULL;
    }
    for (size_t i = 0; i < result_count; i++) {
        if (!signature->result_closure_module_ids[i] ||
            !signature->result_closure_ids[i])
            return NULL;
        for (size_t j = 0; j < i; j++)
            if (signature->result_closure_module_ids[j] ==
                    signature->result_closure_module_ids[i] &&
                signature->result_closure_ids[j] ==
                    signature->result_closure_ids[i])
                return NULL;
    }
    size_t total = domain_count + closure_count + closure_domain_count +
        callable_count + callable_domain_count;
    QttGradeExpr **expressions =
        total ? malloc(total * sizeof(*expressions)) : NULL;
    if (total && !expressions) return NULL;
    if (domain_count)
        memcpy(expressions, signature->domain_expressions,
               domain_count * sizeof(*expressions));
    if (closure_count)
        memcpy(expressions + domain_count, signature->closure_expressions,
               closure_count * sizeof(*expressions));
    if (closure_domain_count)
        memcpy(expressions + domain_count + closure_count,
               signature->closure_domain_expressions,
               closure_domain_count * sizeof(*expressions));
    if (callable_count)
        memcpy(expressions + domain_count + closure_count +
                   closure_domain_count,
               signature->callable_invocation_expressions,
               callable_count * sizeof(*expressions));
    if (callable_domain_count)
        memcpy(expressions + domain_count + closure_count +
                   closure_domain_count + callable_count,
               signature->callable_domain_expressions,
               callable_domain_count * sizeof(*expressions));
    QttGradeScheme *scheme = qtt_grade_generalize_excluding(
        arena, expressions, total, NULL, 0);
    free(expressions);
    if (!scheme) return NULL;
    scheme->domain_count = domain_count;
    scheme->closure_count = closure_count;
    if (closure_count) {
        scheme->closure_module_ids =
            malloc(closure_count * sizeof(*scheme->closure_module_ids));
        scheme->closure_ids =
            malloc(closure_count * sizeof(*scheme->closure_ids));
        scheme->closure_binder_ids =
            malloc(closure_count * sizeof(*scheme->closure_binder_ids));
        scheme->closure_slots =
            malloc(closure_count * sizeof(*scheme->closure_slots));
        scheme->closure_parameter_indices = malloc(
            closure_count * sizeof(*scheme->closure_parameter_indices));
        scheme->closure_origin_kinds = malloc(
            closure_count * sizeof(*scheme->closure_origin_kinds));
        scheme->closure_origin_ids = malloc(
            closure_count * sizeof(*scheme->closure_origin_ids));
        if (!scheme->closure_module_ids ||
            !scheme->closure_ids || !scheme->closure_binder_ids ||
            !scheme->closure_slots ||
            !scheme->closure_parameter_indices ||
            !scheme->closure_origin_kinds || !scheme->closure_origin_ids) {
            qtt_grade_scheme_free(scheme);
            return NULL;
        }
        memcpy(scheme->closure_module_ids, signature->closure_module_ids,
               closure_count * sizeof(*scheme->closure_module_ids));
        memcpy(scheme->closure_ids, signature->closure_ids,
               closure_count * sizeof(*scheme->closure_ids));
        memcpy(scheme->closure_binder_ids, signature->closure_binder_ids,
               closure_count * sizeof(*scheme->closure_binder_ids));
        memcpy(scheme->closure_slots, signature->closure_slots,
               closure_count * sizeof(*scheme->closure_slots));
        memcpy(scheme->closure_parameter_indices,
               signature->closure_parameter_indices,
               closure_count * sizeof(*scheme->closure_parameter_indices));
        for (size_t i = 0; i < closure_count; i++) {
            if (signature->closure_origin_kinds &&
                signature->closure_origin_ids) {
                scheme->closure_origin_kinds[i] =
                    signature->closure_origin_kinds[i];
                scheme->closure_origin_ids[i] =
                    signature->closure_origin_ids[i];
            } else if (signature->closure_parameter_indices[i] != SIZE_MAX) {
                scheme->closure_origin_kinds[i] =
                    QTT_ENVIRONMENT_PARAMETER;
                scheme->closure_origin_ids[i] =
                    signature->closure_parameter_indices[i];
            } else {
                scheme->closure_origin_kinds[i] = QTT_ENVIRONMENT_LOCAL;
                scheme->closure_origin_ids[i] =
                    signature->closure_binder_ids[i];
            }
        }
    }
    scheme->closure_domain_count = closure_domain_count;
    if (closure_domain_count) {
        scheme->closure_domain_module_ids = malloc(
            closure_domain_count *
            sizeof(*scheme->closure_domain_module_ids));
        scheme->closure_domain_ids = malloc(
            closure_domain_count * sizeof(*scheme->closure_domain_ids));
        scheme->closure_domain_indices = malloc(
            closure_domain_count * sizeof(*scheme->closure_domain_indices));
        if (!scheme->closure_domain_module_ids ||
            !scheme->closure_domain_ids ||
            !scheme->closure_domain_indices) {
            qtt_grade_scheme_free(scheme);
            return NULL;
        }
        memcpy(scheme->closure_domain_module_ids,
               signature->closure_domain_module_ids,
               closure_domain_count *
               sizeof(*scheme->closure_domain_module_ids));
        memcpy(scheme->closure_domain_ids,
               signature->closure_domain_ids,
               closure_domain_count * sizeof(*scheme->closure_domain_ids));
        memcpy(scheme->closure_domain_indices,
               signature->closure_domain_indices,
               closure_domain_count *
               sizeof(*scheme->closure_domain_indices));
    }
    scheme->result_closure_count = result_count;
    if (result_count) {
        scheme->result_closure_module_ids = malloc(
            result_count * sizeof(*scheme->result_closure_module_ids));
        scheme->result_closure_ids =
            malloc(result_count * sizeof(*scheme->result_closure_ids));
        if (!scheme->result_closure_module_ids ||
            !scheme->result_closure_ids) {
            qtt_grade_scheme_free(scheme);
            return NULL;
        }
        memcpy(scheme->result_closure_module_ids,
               signature->result_closure_module_ids,
               result_count * sizeof(*scheme->result_closure_module_ids));
        memcpy(scheme->result_closure_ids,
               signature->result_closure_ids,
               result_count * sizeof(*scheme->result_closure_ids));
    }
    scheme->callable_parameter_count = callable_count;
    if (callable_count) {
        scheme->callable_parameter_indices = malloc(
            callable_count * sizeof(*scheme->callable_parameter_indices));
        if (!scheme->callable_parameter_indices) {
            qtt_grade_scheme_free(scheme);
            return NULL;
        }
        memcpy(scheme->callable_parameter_indices,
               signature->callable_parameter_indices,
               callable_count *
               sizeof(*scheme->callable_parameter_indices));
    }
    scheme->callable_domain_count = callable_domain_count;
    if (callable_domain_count) {
        scheme->callable_domain_parameter_indices = malloc(
            callable_domain_count *
            sizeof(*scheme->callable_domain_parameter_indices));
        scheme->callable_domain_indices = malloc(
            callable_domain_count *
            sizeof(*scheme->callable_domain_indices));
        if (!scheme->callable_domain_parameter_indices ||
            !scheme->callable_domain_indices) {
            qtt_grade_scheme_free(scheme);
            return NULL;
        }
        memcpy(scheme->callable_domain_parameter_indices,
               signature->callable_domain_parameter_indices,
               callable_domain_count *
               sizeof(*scheme->callable_domain_parameter_indices));
        memcpy(scheme->callable_domain_indices,
               signature->callable_domain_indices,
               callable_domain_count *
               sizeof(*scheme->callable_domain_indices));
    }
    return scheme;
}

QttGradeScheme *qtt_grade_generalize_excluding(
    QttGradeArena *arena, QttGradeExpr *const *expressions, size_t count,
    const uint64_t *rigid_variables, size_t rigid_count) {
    if (!arena || (count && !expressions) ||
        (rigid_count && !rigid_variables))
        return NULL;
    QttGradeScheme *scheme = calloc(1, sizeof(*scheme));
    if (!scheme) return NULL;
    scheme->arena = grade_arena_retain(arena);
    scheme->references = 1;
    if (count) {
        scheme->expressions = malloc(count * sizeof(*scheme->expressions));
        if (!scheme->expressions) {
            qtt_grade_scheme_free(scheme);
            return NULL;
        }
        memcpy(scheme->expressions, expressions,
               count * sizeof(*scheme->expressions));
    }
    scheme->expression_count = count;
    size_t capacity = 0;
    for (size_t i = 0; i < count; i++)
        if (!collect_variables(
                expressions[i], &scheme->quantified,
                &scheme->quantified_count, &capacity)) {
            qtt_grade_scheme_free(scheme);
            return NULL;
        }
    size_t write = 0;
    for (size_t i = 0; i < scheme->quantified_count; i++)
        if (!id_present(rigid_variables, rigid_count,
                        scheme->quantified[i]))
            scheme->quantified[write++] = scheme->quantified[i];
    scheme->quantified_count = write;
    return scheme;
}

size_t qtt_grade_scheme_quantified_count(const QttGradeScheme *scheme) {
    return scheme ? scheme->quantified_count : 0;
}

size_t qtt_grade_scheme_expression_count(const QttGradeScheme *scheme) {
    return scheme ? scheme->expression_count : 0;
}

size_t qtt_grade_scheme_domain_count(const QttGradeScheme *scheme) {
    return scheme ? scheme->domain_count : 0;
}

size_t qtt_grade_scheme_closure_count(const QttGradeScheme *scheme) {
    return scheme ? scheme->closure_count : 0;
}

uint64_t qtt_grade_scheme_closure_module(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->closure_count
        ? scheme->closure_module_ids[index] : 0;
}

uint64_t qtt_grade_scheme_closure_id(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->closure_count
        ? scheme->closure_ids[index] : 0;
}

uint64_t qtt_grade_scheme_closure_binder(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->closure_count
        ? scheme->closure_binder_ids[index] : 0;
}

size_t qtt_grade_scheme_closure_slot(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->closure_count
        ? scheme->closure_slots[index] : SIZE_MAX;
}

size_t qtt_grade_scheme_closure_parameter_index(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->closure_count
        ? scheme->closure_parameter_indices[index] : SIZE_MAX;
}

QttEnvironmentOriginKind qtt_grade_scheme_closure_origin_kind(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->closure_count
        ? scheme->closure_origin_kinds[index] : QTT_ENVIRONMENT_PARAMETER;
}

uint64_t qtt_grade_scheme_closure_origin_id(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->closure_count
        ? scheme->closure_origin_ids[index] : 0;
}

size_t qtt_grade_scheme_closure_domain_count(
    const QttGradeScheme *scheme) {
    return scheme ? scheme->closure_domain_count : 0;
}

uint64_t qtt_grade_scheme_closure_domain_module(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->closure_domain_count
        ? scheme->closure_domain_module_ids[index] : 0;
}

uint64_t qtt_grade_scheme_closure_domain_id(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->closure_domain_count
        ? scheme->closure_domain_ids[index] : 0;
}

size_t qtt_grade_scheme_closure_domain_index(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->closure_domain_count
        ? scheme->closure_domain_indices[index] : SIZE_MAX;
}

size_t qtt_grade_scheme_result_closure_count(
    const QttGradeScheme *scheme) {
    return scheme ? scheme->result_closure_count : 0;
}

uint64_t qtt_grade_scheme_result_closure_module(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->result_closure_count
        ? scheme->result_closure_module_ids[index] : 0;
}

uint64_t qtt_grade_scheme_result_closure_id(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->result_closure_count
        ? scheme->result_closure_ids[index] : 0;
}

size_t qtt_grade_scheme_callable_parameter_count(
    const QttGradeScheme *scheme) {
    return scheme ? scheme->callable_parameter_count : 0;
}

size_t qtt_grade_scheme_callable_parameter_index(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->callable_parameter_count
        ? scheme->callable_parameter_indices[index] : SIZE_MAX;
}

size_t qtt_grade_scheme_callable_domain_count(
    const QttGradeScheme *scheme) {
    return scheme ? scheme->callable_domain_count : 0;
}

size_t qtt_grade_scheme_callable_domain_parameter_index(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->callable_domain_count
        ? scheme->callable_domain_parameter_indices[index] : SIZE_MAX;
}

size_t qtt_grade_scheme_callable_domain_index(
    const QttGradeScheme *scheme, size_t index) {
    return scheme && index < scheme->callable_domain_count
        ? scheme->callable_domain_indices[index] : SIZE_MAX;
}

QttGradeScheme *qtt_grade_scheme_retain(QttGradeScheme *scheme) {
    if (scheme) scheme->references++;
    return scheme;
}

static QttGradeExpr *instantiate_expression(
    QttGradeArena *arena, const QttGradeScheme *scheme,
    QttGradeExpr *const *fresh, const QttGradeExpr *expression) {
    if (expression->kind == QTT_GRADE_VARIABLE) {
        for (size_t i = 0; i < scheme->quantified_count; i++)
            if (scheme->quantified[i] == expression->variable)
                return fresh[i];
        return (QttGradeExpr *)expression;
    }
    if (expression->kind == QTT_GRADE_CONSTANT)
        return qtt_grade_constant(arena, expression->constant);
    QttGradeExpr *left = instantiate_expression(
        arena, scheme, fresh, expression->left);
    QttGradeExpr *right = instantiate_expression(
        arena, scheme, fresh, expression->right);
    if (expression->kind == QTT_GRADE_ADD)
        return qtt_grade_add(arena, left, right);
    if (expression->kind == QTT_GRADE_MULTIPLY)
        return qtt_grade_multiply(arena, left, right);
    return qtt_grade_maximum(arena, left, right);
}

QttGradeExpr **qtt_grade_instantiate(
    QttGradeArena *arena, const QttGradeScheme *scheme) {
    if (!arena || !scheme) return NULL;
    QttGradeExpr **fresh = scheme->quantified_count
        ? calloc(scheme->quantified_count, sizeof(*fresh)) : NULL;
    QttGradeExpr **result = scheme->expression_count
        ? calloc(scheme->expression_count, sizeof(*result)) : calloc(1, sizeof(*result));
    if ((scheme->quantified_count && !fresh) || !result) {
        free(fresh); free(result); return NULL;
    }
    for (size_t i = 0; i < scheme->quantified_count; i++) {
        fresh[i] = qtt_grade_fresh(arena);
        if (!fresh[i]) {
            free(fresh); free(result); return NULL;
        }
    }
    for (size_t i = 0; i < scheme->expression_count; i++) {
        result[i] = instantiate_expression(
            arena, scheme, fresh, scheme->expressions[i]);
        if (!result[i]) {
            free(fresh); free(result); return NULL;
        }
    }
    free(fresh);
    return result;
}

void qtt_grade_scheme_free(QttGradeScheme *scheme) {
    if (!scheme) return;
    if (--scheme->references) return;
    free(scheme->quantified);
    free(scheme->expressions);
    free(scheme->closure_module_ids);
    free(scheme->closure_ids);
    free(scheme->closure_binder_ids);
    free(scheme->closure_slots);
    free(scheme->closure_parameter_indices);
    free(scheme->closure_origin_kinds);
    free(scheme->closure_origin_ids);
    free(scheme->closure_domain_module_ids);
    free(scheme->closure_domain_ids);
    free(scheme->closure_domain_indices);
    free(scheme->result_closure_module_ids);
    free(scheme->result_closure_ids);
    free(scheme->callable_parameter_indices);
    free(scheme->callable_domain_parameter_indices);
    free(scheme->callable_domain_indices);
    qtt_grade_arena_free(scheme->arena);
    free(scheme);
}
