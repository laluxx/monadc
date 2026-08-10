#ifndef MONAD_QTT_USAGE_H
#define MONAD_QTT_USAGE_H

/* Symbolic, path-sensitive demand analysis. */

#include "../reader.h"

typedef struct QttUsageReport QttUsageReport;

/*
 * Infer path-sensitive syntactic demand for the binders of one lambda.
 * The returned strings are owned by the report and remain valid until free.
 *
 * The graded structure follows:
 * - Moon, Eades, Orchard, "Graded Modal Dependent Type Theory"
 *   https://arxiv.org/abs/2010.13163
 *
 * This is demand analysis, not ownership elaboration. Operational dup/drop
 * insertion belongs to a later linear resource calculus:
 * - Reinking et al., "Perceus: Garbage Free Reference Counting with Reuse"
 *   https://www.microsoft.com/en-us/research/publication/perceus-garbage-free-reference-counting-with-reuse/
 */
QttUsageReport *qtt_usage_analyze_lambda(const AST *lambda);
const char *qtt_usage_format(const QttUsageReport *report,
                             const char *binder_name);
size_t qtt_usage_report_count(const QttUsageReport *report);
const char *qtt_usage_report_name(const QttUsageReport *report, size_t index);
uint64_t qtt_usage_report_binder_id(const QttUsageReport *report,
                                    size_t index);
const char *qtt_usage_report_format(const QttUsageReport *report, size_t index);
void qtt_usage_report_free(QttUsageReport *report);

#endif
