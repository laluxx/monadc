#ifndef MONAD_TOOLING_REWRITE_H
#define MONAD_TOOLING_REWRITE_H

#include "diagnostic.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TOOLING_REWRITE_EMPTY,
    TOOLING_REWRITE_READY,
    TOOLING_REWRITE_CONFLICT,
    TOOLING_REWRITE_INCOMPLETE_EVIDENCE,
    TOOLING_REWRITE_INVALID_SPAN,
    TOOLING_REWRITE_OUT_OF_MEMORY,
    TOOLING_REWRITE_APPLIED,
    TOOLING_REWRITE_STALE_SOURCE,
    TOOLING_REWRITE_NOT_READY,
} ToolingRewriteStatus;

typedef struct {
    size_t start, end;
    char *replacement;
    const Diagnostic *evidence;
} ToolingPlannedEdit;

typedef struct {
    char *path;
    uint64_t source_fingerprint;
    size_t source_length;
    ToolingPlannedEdit *edits;
    size_t edit_count, edit_capacity;
    ToolingRewriteStatus status;
} ToolingRewritePlan;

void tooling_rewrite_plan_init(ToolingRewritePlan *plan);
void tooling_rewrite_plan_dispose(ToolingRewritePlan *plan);

ToolingRewriteStatus tooling_rewrite_plan_build(
    ToolingRewritePlan *plan, const char *path, const char *source,
    const DiagnosticSet *diagnostics);

ToolingRewriteStatus tooling_rewrite_apply(
    const ToolingRewritePlan *plan, const char *source,
    char **rewritten, size_t *rewritten_length);

const char *tooling_rewrite_status_name(ToolingRewriteStatus status);

#endif
