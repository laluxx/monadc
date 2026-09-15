#ifndef MONAD_QTT_CONSTRAINTS_H
#define MONAD_QTT_CONSTRAINTS_H

#include "quantity.h"
#include "environment.h"

#include <stddef.h>

typedef struct QttGradeArena QttGradeArena;
typedef struct QttGradeExpr QttGradeExpr;
typedef struct QttGradeSolver QttGradeSolver;
typedef struct QttGradeScheme QttGradeScheme;
typedef struct {
    QttGradeExpr *const *domain_expressions;
    size_t domain_count;
    const uint64_t *closure_module_ids;
    const uint64_t *closure_ids;
    const uint64_t *closure_binder_ids;
    const size_t *closure_slots;
    const size_t *closure_parameter_indices;
    const QttEnvironmentOriginKind *closure_origin_kinds;
    const uint64_t *closure_origin_ids;
    QttGradeExpr *const *closure_expressions;
    size_t closure_count;
    const uint64_t *closure_domain_module_ids;
    const uint64_t *closure_domain_ids;
    const size_t *closure_domain_indices;
    QttGradeExpr *const *closure_domain_expressions;
    size_t closure_domain_count;
    const uint64_t *result_closure_module_ids;
    const uint64_t *result_closure_ids;
    size_t result_closure_count;
    const size_t *callable_parameter_indices;
    QttGradeExpr *const *callable_invocation_expressions;
    size_t callable_parameter_count;
    const size_t *callable_domain_parameter_indices;
    const size_t *callable_domain_indices;
    QttGradeExpr *const *callable_domain_expressions;
    size_t callable_domain_count;
} QttGradeSignature;

typedef enum {
    QTT_GRADE_CONSTANT,
    QTT_GRADE_VARIABLE,
    QTT_GRADE_ADD,
    QTT_GRADE_MULTIPLY,
    QTT_GRADE_MAXIMUM,
} QttGradeExprKind;

typedef enum {
    QTT_GRADE_SOLVED,
    QTT_GRADE_UNSATISFIABLE,
    QTT_GRADE_OCCURS_CYCLE,
    QTT_GRADE_SOLVER_OUT_OF_MEMORY,
    QTT_GRADE_UNSUPPORTED_CONSTRAINT,
} QttGradeSolveResult;

QttGradeArena *qtt_grade_arena_new(void);
void qtt_grade_arena_free(QttGradeArena *arena);
QttGradeExpr *qtt_grade_constant(
    QttGradeArena *arena, QttQuantity quantity);
QttGradeExpr *qtt_grade_fresh(QttGradeArena *arena);
QttGradeExpr *qtt_grade_add(
    QttGradeArena *arena, QttGradeExpr *left, QttGradeExpr *right);
QttGradeExpr *qtt_grade_multiply(
    QttGradeArena *arena, QttGradeExpr *left, QttGradeExpr *right);
QttGradeExpr *qtt_grade_maximum(
    QttGradeArena *arena, QttGradeExpr *left, QttGradeExpr *right);
QttGradeExpr *qtt_grade_substitute(
    QttGradeArena *arena,
    const QttGradeExpr *expression,
    const QttGradeExpr *variable,
    QttGradeExpr *replacement);
QttGradeExprKind qtt_grade_expr_kind(const QttGradeExpr *expression);
uint64_t qtt_grade_variable_id(const QttGradeExpr *expression);
QttQuantity qtt_grade_constant_value(const QttGradeExpr *expression);

QttGradeSolver *qtt_grade_solver_new(QttGradeArena *arena);
void qtt_grade_solver_free(QttGradeSolver *solver);
bool qtt_grade_constrain_leq(
    QttGradeSolver *solver, QttGradeExpr *left, QttGradeExpr *right);
bool qtt_grade_constrain_equal(
    QttGradeSolver *solver, QttGradeExpr *left, QttGradeExpr *right);
QttGradeSolveResult qtt_grade_solve(QttGradeSolver *solver);
QttQuantity qtt_grade_solution(
    const QttGradeSolver *solver, const QttGradeExpr *expression);

QttGradeScheme *qtt_grade_generalize(
    QttGradeArena *arena, QttGradeExpr *const *expressions, size_t count);
QttGradeScheme *qtt_grade_generalize_signature(
    QttGradeArena *arena, const QttGradeSignature *signature);
QttGradeScheme *qtt_grade_generalize_excluding(
    QttGradeArena *arena,
    QttGradeExpr *const *expressions,
    size_t count,
    const uint64_t *rigid_variables,
    size_t rigid_count);
size_t qtt_grade_scheme_quantified_count(const QttGradeScheme *scheme);
size_t qtt_grade_scheme_expression_count(const QttGradeScheme *scheme);
size_t qtt_grade_scheme_domain_count(const QttGradeScheme *scheme);
size_t qtt_grade_scheme_closure_count(const QttGradeScheme *scheme);
uint64_t qtt_grade_scheme_closure_module(
    const QttGradeScheme *scheme, size_t index);
uint64_t qtt_grade_scheme_closure_id(
    const QttGradeScheme *scheme, size_t index);
uint64_t qtt_grade_scheme_closure_binder(
    const QttGradeScheme *scheme, size_t index);
size_t qtt_grade_scheme_closure_slot(
    const QttGradeScheme *scheme, size_t index);
size_t qtt_grade_scheme_closure_parameter_index(
    const QttGradeScheme *scheme, size_t index);
QttEnvironmentOriginKind qtt_grade_scheme_closure_origin_kind(
    const QttGradeScheme *scheme, size_t index);
uint64_t qtt_grade_scheme_closure_origin_id(
    const QttGradeScheme *scheme, size_t index);
size_t qtt_grade_scheme_closure_domain_count(
    const QttGradeScheme *scheme);
uint64_t qtt_grade_scheme_closure_domain_module(
    const QttGradeScheme *scheme, size_t index);
uint64_t qtt_grade_scheme_closure_domain_id(
    const QttGradeScheme *scheme, size_t index);
size_t qtt_grade_scheme_closure_domain_index(
    const QttGradeScheme *scheme, size_t index);
size_t qtt_grade_scheme_result_closure_count(
    const QttGradeScheme *scheme);
uint64_t qtt_grade_scheme_result_closure_module(
    const QttGradeScheme *scheme, size_t index);
uint64_t qtt_grade_scheme_result_closure_id(
    const QttGradeScheme *scheme, size_t index);
size_t qtt_grade_scheme_callable_parameter_count(
    const QttGradeScheme *scheme);
size_t qtt_grade_scheme_callable_parameter_index(
    const QttGradeScheme *scheme, size_t index);
size_t qtt_grade_scheme_callable_domain_count(
    const QttGradeScheme *scheme);
size_t qtt_grade_scheme_callable_domain_parameter_index(
    const QttGradeScheme *scheme, size_t index);
size_t qtt_grade_scheme_callable_domain_index(
    const QttGradeScheme *scheme, size_t index);
QttGradeScheme *qtt_grade_scheme_retain(QttGradeScheme *scheme);
QttGradeExpr **qtt_grade_instantiate(
    QttGradeArena *arena, const QttGradeScheme *scheme);
void qtt_grade_scheme_free(QttGradeScheme *scheme);

#endif
