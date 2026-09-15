#include "coherence.h"

#include "constraints.h"
#include "core_usage.h"
#include "demand.h"

QttCoherenceReport qtt_coherence_check(
    const QttCoreNode *core,
    const QttCoreVar *subjects,
    size_t subject_count) {
    QttCoherenceReport report = {
        .status = QTT_COHERENCE_INVALID_INPUT,
        .boundary = QTT_COHERENCE_BOUNDARY_NONE,
    };
    if (!core || (subject_count && !subjects)) return report;
    QttGradeArena *arena = qtt_grade_arena_new();
    if (!arena) {
        report.status = QTT_COHERENCE_OUT_OF_MEMORY;
        return report;
    }
    QttCoreUsageResult lowered = qtt_core_usage_lower(arena, core);
    if (lowered.status == QTT_CORE_USAGE_UNSUPPORTED) {
        report.status = QTT_COHERENCE_OUTSIDE_COMMON_FRAGMENT;
        report.boundary = QTT_COHERENCE_BOUNDARY_APPLICATION;
        report.offending = lowered.offending;
        qtt_grade_arena_free(arena);
        return report;
    }
    if (lowered.status == QTT_CORE_USAGE_INVALID) {
        qtt_grade_arena_free(arena);
        return report;
    }
    QttDemandError error = QTT_DEMAND_OK;
    QttDemandCertificate *legacy = qtt_demand_derive(core, &error);
    QttGradeSolver *solver = qtt_grade_solver_new(arena);
    if (!legacy || lowered.status != QTT_CORE_USAGE_OK || !solver ||
        qtt_grade_solve(solver) != QTT_GRADE_SOLVED) {
        report.status = QTT_COHERENCE_OUT_OF_MEMORY;
        qtt_demand_certificate_free(legacy);
        qtt_grade_solver_free(solver);
        qtt_usage_context_free(lowered.usage);
        qtt_grade_arena_free(arena);
        return report;
    }
    report.status = QTT_COHERENCE_AGREES;
    for (size_t i = 0; i < subject_count; i++) {
        report.subject = subjects[i];
        report.legacy = qtt_demand_runtime(legacy, core, subjects[i]);
        QttGradeExpr *grade =
            qtt_usage_grade(lowered.usage, subjects[i].binder_id);
        report.symbolic = grade
            ? qtt_grade_solution(solver, grade)
            : qtt_quantity_finite(0);
        report.checked_binder_count++;
        if (!qtt_quantity_equal(report.legacy, report.symbolic)) {
            report.status = QTT_COHERENCE_DISAGREES;
            break;
        }
    }
    qtt_demand_certificate_free(legacy);
    qtt_grade_solver_free(solver);
    qtt_usage_context_free(lowered.usage);
    qtt_grade_arena_free(arena);
    return report;
}

QttCoherenceReport qtt_coherence_check_semantic(
    QttSemanticFunction *function,
    const QttCoreNode *core,
    const QttCoreVar *subjects,
    size_t subject_count) {
    QttCoherenceReport report = {
        .status = QTT_COHERENCE_INVALID_INPUT,
        .boundary = QTT_COHERENCE_BOUNDARY_NONE,
    };
    if (!function || !core || (subject_count && !subjects) ||
        qtt_semantic_ir_verify(function, core) !=
            QTT_SEMANTIC_IR_VALID)
        return report;
    QttDemandError error = QTT_DEMAND_OK;
    QttDemandCertificate *legacy =
        qtt_demand_derive(core, &error);
    if (!legacy) {
        report.status = error == QTT_DEMAND_OUT_OF_MEMORY
            ? QTT_COHERENCE_OUT_OF_MEMORY
            : QTT_COHERENCE_INVALID_INPUT;
        return report;
    }
    report.status = QTT_COHERENCE_AGREES;
    for (size_t i = 0; i < subject_count; i++) {
        report.subject = subjects[i];
        report.legacy =
            qtt_demand_runtime(legacy, core, subjects[i]);
        if (!qtt_semantic_ir_solved_usage_for(
                function, subjects[i], &report.symbolic)) {
            report.status = QTT_COHERENCE_INVALID_INPUT;
            break;
        }
        report.checked_binder_count++;
        if (!qtt_quantity_equal(report.legacy, report.symbolic)) {
            report.status = QTT_COHERENCE_DISAGREES;
            break;
        }
    }
    qtt_demand_certificate_free(legacy);
    return report;
}

const char *qtt_coherence_boundary_name(QttCoherenceBoundary boundary) {
    return boundary == QTT_COHERENCE_BOUNDARY_NONE
        ? "none" : "application";
}
