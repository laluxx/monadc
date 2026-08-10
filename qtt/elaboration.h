#ifndef MONAD_QTT_ELABORATION_H
#define MONAD_QTT_ELABORATION_H

/*
 * Symbolic usage contexts for quantitative elaboration.
 *
 * Sequential evaluation combines demands additively. Exclusive alternatives
 * combine them by join (maximum). Passing an expression to a domain graded rho
 * scales every free-binder demand in that expression by rho.
 */

#include "constraints.h"

#include <stddef.h>
#include <stdint.h>

typedef struct QttUsageContext QttUsageContext;
typedef struct QttUsageClosure QttUsageClosure;

QttUsageContext *qtt_usage_empty(QttGradeArena *arena);
QttUsageContext *qtt_usage_singleton(
    QttGradeArena *arena, uint64_t binder_id);
QttUsageContext *qtt_usage_sequence(
    QttGradeArena *arena,
    const QttUsageContext *left,
    const QttUsageContext *right);
QttUsageContext *qtt_usage_choice(
    QttGradeArena *arena,
    const QttUsageContext *left,
    const QttUsageContext *right);
QttUsageContext *qtt_usage_scale(
    QttGradeArena *arena,
    QttGradeExpr *factor,
    const QttUsageContext *usage);
QttUsageContext *qtt_usage_application(
    QttGradeArena *arena,
    const QttUsageContext *callee,
    const QttUsageContext *const *arguments,
    QttGradeExpr *const *domain_grades,
    size_t argument_count);
QttUsageContext *qtt_usage_let(
    QttGradeArena *arena,
    uint64_t binder_id,
    const QttUsageContext *value,
    const QttUsageContext *body);
QttUsageClosure *qtt_usage_closure(
    QttGradeArena *arena,
    const uint64_t *parameter_ids,
    size_t parameter_count,
    const QttUsageContext *body);
const QttUsageContext *qtt_usage_closure_captures(
    const QttUsageClosure *closure);
const QttUsageContext *qtt_usage_closure_latent(
    const QttUsageClosure *closure);
QttUsageContext *qtt_usage_closure_invoke(
    QttGradeArena *arena,
    const QttUsageClosure *closure,
    QttGradeExpr *invocation_grade);
void qtt_usage_closure_free(QttUsageClosure *closure);

size_t qtt_usage_binding_count(const QttUsageContext *usage);
uint64_t qtt_usage_binding_id(
    const QttUsageContext *usage, size_t index);
QttGradeExpr *qtt_usage_binding_grade(
    const QttUsageContext *usage, size_t index);
QttGradeExpr *qtt_usage_grade(
    const QttUsageContext *usage, uint64_t binder_id);
bool qtt_usage_constrain_binder(
    QttGradeSolver *solver,
    const QttUsageContext *usage,
    uint64_t binder_id,
    QttGradeExpr *allowance);
void qtt_usage_context_free(QttUsageContext *usage);

#endif
