#include "rewrite.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t source_fingerprint(const char *source) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)source; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static char *copy_text(const char *text) {
    if (!text) return NULL;
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}

void tooling_rewrite_plan_init(ToolingRewritePlan *plan) {
    if (plan) memset(plan, 0, sizeof(*plan));
}

void tooling_rewrite_plan_dispose(ToolingRewritePlan *plan) {
    if (!plan) return;
    free(plan->path);
    for (size_t i = 0; i < plan->edit_count; i++)
        free(plan->edits[i].replacement);
    free(plan->edits);
    memset(plan, 0, sizeof(*plan));
}

static bool source_offset(const char *source, size_t line, size_t column,
                          size_t *offset) {
    if (!source || !line || !column || !offset) return false;
    const char *cursor = source;
    for (size_t current = 1; current < line; current++) {
        cursor = strchr(cursor, '\n');
        if (!cursor) return false;
        cursor++;
    }
    size_t line_length = strcspn(cursor, "\n");
    if (column - 1 > line_length) return false;
    *offset = (size_t)(cursor - source) + column - 1;
    return true;
}

static bool append_edit(ToolingRewritePlan *plan, size_t start, size_t end,
                        const char *replacement,
                        const Diagnostic *evidence) {
    if (plan->edit_count == plan->edit_capacity) {
        size_t capacity = plan->edit_capacity ? plan->edit_capacity * 2 : 8;
        if (capacity < plan->edit_capacity) return false;
        void *grown = realloc(plan->edits, capacity * sizeof(*plan->edits));
        if (!grown) return false;
        plan->edits = grown;
        plan->edit_capacity = capacity;
    }
    char *owned = copy_text(replacement);
    if (!owned) return false;
    plan->edits[plan->edit_count++] =
        (ToolingPlannedEdit){start, end, owned, evidence};
    return true;
}

static int compare_edit(const void *left_pointer, const void *right_pointer) {
    const ToolingPlannedEdit *left = left_pointer;
    const ToolingPlannedEdit *right = right_pointer;
    if (left->start != right->start) return left->start < right->start ? -1 : 1;
    if (left->end != right->end) return left->end < right->end ? -1 : 1;
    return strcmp(left->replacement, right->replacement);
}

static bool same_edit(const ToolingPlannedEdit *left,
                      const ToolingPlannedEdit *right) {
    return left->start == right->start && left->end == right->end &&
           strcmp(left->replacement, right->replacement) == 0;
}

ToolingRewriteStatus tooling_rewrite_plan_build(
    ToolingRewritePlan *plan, const char *path, const char *source,
    const DiagnosticSet *diagnostics) {
    if (!plan || !path || !source || !diagnostics)
        return TOOLING_REWRITE_INVALID_SPAN;
    tooling_rewrite_plan_dispose(plan);
    plan->path = copy_text(path);
    if (!plan->path) return plan->status = TOOLING_REWRITE_OUT_OF_MEMORY;
    plan->source_length = strlen(source);
    plan->source_fingerprint = source_fingerprint(source);

    for (size_t i = 0; i < diagnostics->count; i++) {
        const Diagnostic *diagnostic = &diagnostics->items[i];
        for (size_t j = 0; j < diagnostic->edit_count; j++) {
            const DiagnosticEdit *edit = &diagnostic->edits[j];
            if (edit->applicability != DIAGNOSTIC_EDIT_MACHINE_APPLICABLE ||
                !edit->span.path || strcmp(edit->span.path, path) != 0)
                continue;
            if (!diagnostic->complete)
                return plan->status = TOOLING_REWRITE_INCOMPLETE_EVIDENCE;
            size_t start = 0, end = 0;
            if (!source_offset(source, edit->span.line, edit->span.column,
                               &start) ||
                !source_offset(source, edit->span.end_line,
                               edit->span.end_column, &end) ||
                start > end || end > plan->source_length) {
                return plan->status = TOOLING_REWRITE_INVALID_SPAN;
            }
            if (!append_edit(plan, start, end, edit->replacement, diagnostic))
                return plan->status = TOOLING_REWRITE_OUT_OF_MEMORY;
        }
    }
    if (!plan->edit_count) return plan->status = TOOLING_REWRITE_EMPTY;

    qsort(plan->edits, plan->edit_count, sizeof(*plan->edits), compare_edit);
    for (size_t i = 1; i < plan->edit_count; i++) {
        if (same_edit(&plan->edits[i - 1], &plan->edits[i])) continue;
        size_t previous = i - 1;
        while (previous > 0 &&
               same_edit(&plan->edits[previous - 1], &plan->edits[previous]))
            previous--;
        bool same_insertion =
            plan->edits[i].start == plan->edits[i].end &&
            plan->edits[previous].start == plan->edits[previous].end &&
            plan->edits[i].start == plan->edits[previous].start;
        if (same_insertion ||
            plan->edits[i].start < plan->edits[previous].end)
            return plan->status = TOOLING_REWRITE_CONFLICT;
    }
    size_t kept = 0;
    for (size_t i = 0; i < plan->edit_count; i++) {
        if (kept && same_edit(&plan->edits[kept - 1], &plan->edits[i])) {
            free(plan->edits[i].replacement);
            plan->edits[i].replacement = NULL;
            continue;
        }
        if (kept != i) {
            plan->edits[kept] = plan->edits[i];
            plan->edits[i].replacement = NULL;
        }
        kept++;
    }
    plan->edit_count = kept;
    return plan->status = TOOLING_REWRITE_READY;
}

ToolingRewriteStatus tooling_rewrite_apply(
    const ToolingRewritePlan *plan, const char *source,
    char **rewritten, size_t *rewritten_length) {
    if (rewritten) *rewritten = NULL;
    if (rewritten_length) *rewritten_length = 0;
    if (!plan || !source || !rewritten || !rewritten_length ||
        plan->status != TOOLING_REWRITE_READY)
        return TOOLING_REWRITE_NOT_READY;
    if (strlen(source) != plan->source_length ||
        source_fingerprint(source) != plan->source_fingerprint)
        return TOOLING_REWRITE_STALE_SOURCE;

    size_t length = plan->source_length;
    for (size_t i = 0; i < plan->edit_count; i++) {
        size_t removed = plan->edits[i].end - plan->edits[i].start;
        size_t added = strlen(plan->edits[i].replacement);
        if (added > removed && length > SIZE_MAX - (added - removed))
            return TOOLING_REWRITE_OUT_OF_MEMORY;
        length = length - removed + added;
    }
    char *output = malloc(length + 1);
    if (!output) return TOOLING_REWRITE_OUT_OF_MEMORY;
    size_t input_cursor = 0, output_cursor = 0;
    for (size_t i = 0; i < plan->edit_count; i++) {
        const ToolingPlannedEdit *edit = &plan->edits[i];
        size_t unchanged = edit->start - input_cursor;
        memcpy(output + output_cursor, source + input_cursor, unchanged);
        output_cursor += unchanged;
        size_t replacement_length = strlen(edit->replacement);
        memcpy(output + output_cursor, edit->replacement, replacement_length);
        output_cursor += replacement_length;
        input_cursor = edit->end;
    }
    memcpy(output + output_cursor, source + input_cursor,
           plan->source_length - input_cursor);
    output[length] = '\0';
    *rewritten = output;
    *rewritten_length = length;
    return TOOLING_REWRITE_APPLIED;
}

const char *tooling_rewrite_status_name(ToolingRewriteStatus status) {
    static const char *names[] = {
        "empty", "ready", "conflict", "incomplete-evidence", "invalid-span",
        "out-of-memory", "applied", "stale-source", "not-ready"
    };
    return status <= TOOLING_REWRITE_NOT_READY ? names[status] : "unknown";
}
