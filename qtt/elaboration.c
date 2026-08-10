#include "elaboration.h"

#include <stdlib.h>

typedef struct {
    uint64_t binder_id;
    QttGradeExpr *grade;
} QttUsageBinding;

struct QttUsageContext {
    QttGradeArena *arena;
    QttUsageBinding *bindings;
    size_t count;
};

struct QttUsageClosure {
    QttUsageContext *captures;
    QttUsageContext *latent;
};

QttUsageContext *qtt_usage_empty(QttGradeArena *arena) {
    if (!arena) return NULL;
    QttUsageContext *usage = calloc(1, sizeof(*usage));
    if (usage) usage->arena = arena;
    return usage;
}

QttUsageContext *qtt_usage_singleton(
    QttGradeArena *arena, uint64_t binder_id) {
    QttUsageContext *usage = qtt_usage_empty(arena);
    if (!usage) return NULL;
    usage->bindings = malloc(sizeof(*usage->bindings));
    if (!usage->bindings) {
        free(usage);
        return NULL;
    }
    QttGradeExpr *one =
        qtt_grade_constant(arena, qtt_quantity_finite(1));
    if (!one) {
        free(usage->bindings);
        free(usage);
        return NULL;
    }
    usage->bindings[0] = (QttUsageBinding){binder_id, one};
    usage->count = 1;
    return usage;
}

QttGradeExpr *qtt_usage_grade(
    const QttUsageContext *usage, uint64_t binder_id) {
    if (!usage) return NULL;
    for (size_t i = 0; i < usage->count; i++)
        if (usage->bindings[i].binder_id == binder_id)
            return usage->bindings[i].grade;
    return NULL;
}

static bool append(
    QttUsageContext *usage, uint64_t binder_id, QttGradeExpr *grade) {
    QttUsageBinding *grown = realloc(
        usage->bindings, (usage->count + 1) * sizeof(*grown));
    if (!grown) return false;
    usage->bindings = grown;
    usage->bindings[usage->count++] =
        (QttUsageBinding){binder_id, grade};
    return true;
}

typedef QttGradeExpr *(*Combine)(
    QttGradeArena *, QttGradeExpr *, QttGradeExpr *);

static QttUsageContext *combine(
    QttGradeArena *arena,
    const QttUsageContext *left,
    const QttUsageContext *right,
    Combine operation) {
    if (!arena || !left || !right || !operation) return NULL;
    QttUsageContext *result = qtt_usage_empty(arena);
    if (!result) return NULL;
    QttGradeExpr *zero =
        qtt_grade_constant(arena, qtt_quantity_finite(0));
    if (!zero) {
        qtt_usage_context_free(result);
        return NULL;
    }
    for (size_t i = 0; i < left->count; i++) {
        QttGradeExpr *other =
            qtt_usage_grade(right, left->bindings[i].binder_id);
        QttGradeExpr *grade = operation(
            arena, left->bindings[i].grade, other ? other : zero);
        if (!grade || !append(
                result, left->bindings[i].binder_id, grade)) {
            qtt_usage_context_free(result);
            return NULL;
        }
    }
    for (size_t i = 0; i < right->count; i++) {
        if (qtt_usage_grade(left, right->bindings[i].binder_id)) continue;
        QttGradeExpr *grade =
            operation(arena, zero, right->bindings[i].grade);
        if (!grade || !append(
                result, right->bindings[i].binder_id, grade)) {
            qtt_usage_context_free(result);
            return NULL;
        }
    }
    return result;
}

QttUsageContext *qtt_usage_sequence(
    QttGradeArena *arena,
    const QttUsageContext *left,
    const QttUsageContext *right) {
    return combine(arena, left, right, qtt_grade_add);
}

QttUsageContext *qtt_usage_choice(
    QttGradeArena *arena,
    const QttUsageContext *left,
    const QttUsageContext *right) {
    return combine(arena, left, right, qtt_grade_maximum);
}

QttUsageContext *qtt_usage_scale(
    QttGradeArena *arena,
    QttGradeExpr *factor,
    const QttUsageContext *usage) {
    if (!arena || !factor || !usage) return NULL;
    QttUsageContext *result = qtt_usage_empty(arena);
    if (!result) return NULL;
    if (qtt_grade_expr_kind(factor) == QTT_GRADE_CONSTANT &&
        qtt_quantity_is_zero(qtt_grade_constant_value(factor)))
        return result;
    for (size_t i = 0; i < usage->count; i++) {
        QttGradeExpr *grade = qtt_grade_multiply(
            arena, factor, usage->bindings[i].grade);
        if (!grade || !append(
                result, usage->bindings[i].binder_id, grade)) {
            qtt_usage_context_free(result);
            return NULL;
        }
    }
    return result;
}

QttUsageContext *qtt_usage_application(
    QttGradeArena *arena,
    const QttUsageContext *callee,
    const QttUsageContext *const *arguments,
    QttGradeExpr *const *domain_grades,
    size_t argument_count) {
    if (!arena || !callee ||
        (argument_count && (!arguments || !domain_grades)))
        return NULL;
    QttUsageContext *empty = qtt_usage_empty(arena);
    QttUsageContext *result =
        empty ? qtt_usage_sequence(arena, callee, empty) : NULL;
    qtt_usage_context_free(empty);
    if (!result) return NULL;

    for (size_t i = 0; i < argument_count; i++) {
        if (!arguments[i] || !domain_grades[i]) {
            qtt_usage_context_free(result);
            return NULL;
        }
        QttUsageContext *scaled = qtt_usage_scale(
            arena, domain_grades[i], arguments[i]);
        QttUsageContext *next = scaled
            ? qtt_usage_sequence(arena, result, scaled) : NULL;
        qtt_usage_context_free(scaled);
        qtt_usage_context_free(result);
        if (!next) return NULL;
        result = next;
    }
    return result;
}

QttUsageContext *qtt_usage_let(
    QttGradeArena *arena,
    uint64_t binder_id,
    const QttUsageContext *value,
    const QttUsageContext *body) {
    if (!arena || !value || !body || !binder_id) return NULL;
    QttGradeExpr *uses = qtt_usage_grade(body, binder_id);
    if (!uses)
        uses = qtt_grade_constant(arena, qtt_quantity_finite(0));
    QttUsageContext *remaining = qtt_usage_empty(arena);
    if (!uses || !remaining) {
        qtt_usage_context_free(remaining);
        return NULL;
    }
    for (size_t i = 0; i < body->count; i++) {
        if (body->bindings[i].binder_id == binder_id) continue;
        if (!append(remaining, body->bindings[i].binder_id,
                    body->bindings[i].grade)) {
            qtt_usage_context_free(remaining);
            return NULL;
        }
    }
    QttUsageContext *scaled = qtt_usage_scale(arena, uses, value);
    QttUsageContext *result = scaled
        ? qtt_usage_sequence(arena, remaining, scaled) : NULL;
    qtt_usage_context_free(scaled);
    qtt_usage_context_free(remaining);
    return result;
}

static bool binder_present(
    const uint64_t *binders, size_t count, uint64_t binder_id) {
    for (size_t i = 0; i < count; i++)
        if (binders[i] == binder_id) return true;
    return false;
}

QttUsageClosure *qtt_usage_closure(
    QttGradeArena *arena,
    const uint64_t *parameter_ids,
    size_t parameter_count,
    const QttUsageContext *body) {
    if (!arena || !body || (parameter_count && !parameter_ids))
        return NULL;
    QttUsageClosure *closure = calloc(1, sizeof(*closure));
    if (!closure) return NULL;
    closure->captures = qtt_usage_empty(arena);
    closure->latent = qtt_usage_empty(arena);
    QttGradeExpr *one =
        qtt_grade_constant(arena, qtt_quantity_finite(1));
    if (!closure->captures || !closure->latent || !one) {
        qtt_usage_closure_free(closure);
        return NULL;
    }
    for (size_t i = 0; i < body->count; i++) {
        uint64_t binder_id = body->bindings[i].binder_id;
        if (binder_present(parameter_ids, parameter_count, binder_id))
            continue;
        if (!append(closure->latent, binder_id,
                    body->bindings[i].grade) ||
            !append(closure->captures, binder_id, one)) {
            qtt_usage_closure_free(closure);
            return NULL;
        }
    }
    return closure;
}

const QttUsageContext *qtt_usage_closure_captures(
    const QttUsageClosure *closure) {
    return closure ? closure->captures : NULL;
}

const QttUsageContext *qtt_usage_closure_latent(
    const QttUsageClosure *closure) {
    return closure ? closure->latent : NULL;
}

QttUsageContext *qtt_usage_closure_invoke(
    QttGradeArena *arena,
    const QttUsageClosure *closure,
    QttGradeExpr *invocation_grade) {
    if (!closure) return NULL;
    return qtt_usage_scale(
        arena, invocation_grade, closure->latent);
}

void qtt_usage_closure_free(QttUsageClosure *closure) {
    if (!closure) return;
    qtt_usage_context_free(closure->captures);
    qtt_usage_context_free(closure->latent);
    free(closure);
}

size_t qtt_usage_binding_count(const QttUsageContext *usage) {
    return usage ? usage->count : 0;
}

uint64_t qtt_usage_binding_id(
    const QttUsageContext *usage, size_t index) {
    return usage && index < usage->count
        ? usage->bindings[index].binder_id : 0;
}

QttGradeExpr *qtt_usage_binding_grade(
    const QttUsageContext *usage, size_t index) {
    return usage && index < usage->count
        ? usage->bindings[index].grade : NULL;
}

bool qtt_usage_constrain_binder(
    QttGradeSolver *solver,
    const QttUsageContext *usage,
    uint64_t binder_id,
    QttGradeExpr *allowance) {
    if (!solver || !usage || !allowance) return false;
    QttGradeExpr *observed = qtt_usage_grade(usage, binder_id);
    if (!observed)
        observed = qtt_grade_constant(
            usage->arena, qtt_quantity_finite(0));
    return observed &&
        qtt_grade_constrain_leq(solver, observed, allowance);
}

void qtt_usage_context_free(QttUsageContext *usage) {
    if (!usage) return;
    free(usage->bindings);
    free(usage);
}
