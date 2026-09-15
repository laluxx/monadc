#include "diagnostic.h"
#include "rewrite.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static char *owned(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    assert(copy);
    memcpy(copy, text, length + 1);
    return copy;
}

static Diagnostic diagnostic_with_edit(const char *path,
                                       size_t column, size_t end_column,
                                       const char *replacement) {
    Diagnostic diagnostic = {0};
    diagnostic.path = owned(path);
    diagnostic.code = owned("test/rewrite");
    diagnostic.message = owned("rewrite test");
    diagnostic.complete = true;
    assert(diagnostic_add_edit(&diagnostic, path, 1, column, 1, end_column,
                               replacement,
                               DIAGNOSTIC_EDIT_MACHINE_APPLICABLE));
    return diagnostic;
}

static void ordered_edits_are_diagnostic_order_independent(void) {
    const char *path = "Rewrite.mon";
    const char *source = "alpha beta gamma\n";
    DiagnosticSet diagnostics;
    diagnostic_set_init(&diagnostics);
    Diagnostic right = diagnostic_with_edit(path, 12, 17, "delta");
    Diagnostic left = diagnostic_with_edit(path, 1, 6, "A");
    assert(diagnostic_set_push(&diagnostics, &right));
    assert(diagnostic_set_push(&diagnostics, &left));

    ToolingRewritePlan plan;
    tooling_rewrite_plan_init(&plan);
    assert(tooling_rewrite_plan_build(&plan, path, source, &diagnostics) ==
           TOOLING_REWRITE_READY);
    char *rewritten = NULL;
    size_t length = 0;
    assert(tooling_rewrite_apply(&plan, source, &rewritten, &length) ==
           TOOLING_REWRITE_APPLIED);
    assert(length == strlen("A beta delta\n"));
    assert(strcmp(rewritten, "A beta delta\n") == 0);

    free(rewritten);
    tooling_rewrite_plan_dispose(&plan);
    diagnostic_set_dispose(&diagnostics);
}

static void overlapping_edits_are_rejected_transactionally(void) {
    const char *path = "Conflict.mon";
    const char *source = "abcdef\n";
    DiagnosticSet diagnostics;
    diagnostic_set_init(&diagnostics);
    Diagnostic outer = diagnostic_with_edit(path, 2, 6, "x");
    Diagnostic inner = diagnostic_with_edit(path, 4, 7, "y");
    assert(diagnostic_set_push(&diagnostics, &outer));
    assert(diagnostic_set_push(&diagnostics, &inner));

    ToolingRewritePlan plan;
    tooling_rewrite_plan_init(&plan);
    assert(tooling_rewrite_plan_build(&plan, path, source, &diagnostics) ==
           TOOLING_REWRITE_CONFLICT);
    char *rewritten = NULL;
    size_t length = 0;
    assert(tooling_rewrite_apply(&plan, source, &rewritten, &length) ==
           TOOLING_REWRITE_NOT_READY);
    assert(rewritten == NULL);

    tooling_rewrite_plan_dispose(&plan);
    diagnostic_set_dispose(&diagnostics);
}

static void stale_source_is_rejected(void) {
    const char *path = "Stale.mon";
    const char *source = "before\n";
    DiagnosticSet diagnostics;
    diagnostic_set_init(&diagnostics);
    Diagnostic edit = diagnostic_with_edit(path, 1, 7, "after");
    assert(diagnostic_set_push(&diagnostics, &edit));

    ToolingRewritePlan plan;
    tooling_rewrite_plan_init(&plan);
    assert(tooling_rewrite_plan_build(&plan, path, source, &diagnostics) ==
           TOOLING_REWRITE_READY);
    char *rewritten = NULL;
    size_t length = 0;
    assert(tooling_rewrite_apply(&plan, "changed\n", &rewritten, &length) ==
           TOOLING_REWRITE_STALE_SOURCE);
    assert(rewritten == NULL);

    tooling_rewrite_plan_dispose(&plan);
    diagnostic_set_dispose(&diagnostics);
}

static void incomplete_evidence_cannot_drive_unattended_fixes(void) {
    const char *path = "Incomplete.mon";
    DiagnosticSet diagnostics;
    diagnostic_set_init(&diagnostics);
    Diagnostic edit = diagnostic_with_edit(path, 1, 2, "y");
    edit.complete = false;
    assert(diagnostic_set_push(&diagnostics, &edit));

    ToolingRewritePlan plan;
    tooling_rewrite_plan_init(&plan);
    assert(tooling_rewrite_plan_build(&plan, path, "x\n", &diagnostics) ==
           TOOLING_REWRITE_INCOMPLETE_EVIDENCE);
    tooling_rewrite_plan_dispose(&plan);
    diagnostic_set_dispose(&diagnostics);
}

int main(void) {
    ordered_edits_are_diagnostic_order_independent();
    overlapping_edits_are_rejected_transactionally();
    stale_source_is_rejected();
    incomplete_evidence_cannot_drive_unattended_fixes();
    return 0;
}
