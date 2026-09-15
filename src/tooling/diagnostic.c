#include "diagnostic.h"

#include <stdlib.h>
#include <string.h>

static char *diagnostic_copy(const char *text) {
    if (!text) return NULL;
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}

static void diagnostic_span_dispose(DiagnosticSpan *span) {
    free(span->path);
}

void diagnostic_set_init(DiagnosticSet *set) {
    if (set) memset(set, 0, sizeof(*set));
}

void diagnostic_dispose(Diagnostic *diagnostic) {
    if (!diagnostic) return;
    free(diagnostic->path);
    free(diagnostic->code);
    free(diagnostic->message);
    free(diagnostic->explanation);
    for (size_t j = 0; j < diagnostic->edit_count; j++) {
        diagnostic_span_dispose(&diagnostic->edits[j].span);
        free(diagnostic->edits[j].replacement);
    }
    free(diagnostic->edits);
    for (size_t j = 0; j < diagnostic->related_count; j++) {
        diagnostic_span_dispose(&diagnostic->related[j].span);
        free(diagnostic->related[j].message);
    }
    free(diagnostic->related);
    memset(diagnostic, 0, sizeof(*diagnostic));
}

void diagnostic_set_dispose(DiagnosticSet *set) {
    if (!set) return;
    for (size_t i = 0; i < set->count; i++)
        diagnostic_dispose(&set->items[i]);
    free(set->items);
    memset(set, 0, sizeof(*set));
}

bool diagnostic_set_push(DiagnosticSet *set, Diagnostic *diagnostic) {
    if (!set || !diagnostic) return false;
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2 : 16;
        void *items = realloc(set->items, capacity * sizeof(*set->items));
        if (!items) return false;
        set->items = items;
        set->capacity = capacity;
    }
    set->items[set->count++] = *diagnostic;
    memset(diagnostic, 0, sizeof(*diagnostic));
    return true;
}

bool diagnostic_add_edit(Diagnostic *diagnostic, const char *path,
                         size_t line, size_t column,
                         size_t end_line, size_t end_column,
                         const char *replacement,
                         DiagnosticApplicability applicability) {
    if (!diagnostic || !path || !replacement) return false;
    if (diagnostic->edit_count == diagnostic->edit_capacity) {
        size_t capacity = diagnostic->edit_capacity
            ? diagnostic->edit_capacity * 2 : 2;
        void *items = realloc(diagnostic->edits,
                              capacity * sizeof(*diagnostic->edits));
        if (!items) return false;
        diagnostic->edits = items;
        diagnostic->edit_capacity = capacity;
    }
    DiagnosticEdit edit = {0};
    edit.span.path = diagnostic_copy(path);
    edit.span.line = line;
    edit.span.column = column;
    edit.span.end_line = end_line;
    edit.span.end_column = end_column;
    edit.replacement = diagnostic_copy(replacement);
    edit.applicability = applicability;
    if (!edit.span.path || !edit.replacement) {
        diagnostic_span_dispose(&edit.span);
        free(edit.replacement);
        return false;
    }
    diagnostic->edits[diagnostic->edit_count++] = edit;
    return true;
}

bool diagnostic_add_related(Diagnostic *diagnostic, const char *path,
                            size_t line, size_t column,
                            size_t end_line, size_t end_column,
                            const char *message) {
    if (!diagnostic || !path || !message) return false;
    if (diagnostic->related_count == diagnostic->related_capacity) {
        size_t capacity = diagnostic->related_capacity
            ? diagnostic->related_capacity * 2 : 2;
        void *items = realloc(diagnostic->related,
                              capacity * sizeof(*diagnostic->related));
        if (!items) return false;
        diagnostic->related = items;
        diagnostic->related_capacity = capacity;
    }
    DiagnosticRelated related = {0};
    related.span.path = diagnostic_copy(path);
    related.span.line = line;
    related.span.column = column;
    related.span.end_line = end_line;
    related.span.end_column = end_column;
    related.message = diagnostic_copy(message);
    if (!related.span.path || !related.message) {
        diagnostic_span_dispose(&related.span);
        free(related.message);
        return false;
    }
    diagnostic->related[diagnostic->related_count++] = related;
    return true;
}

const char *diagnostic_severity_name(DiagnosticSeverity severity) {
    static const char *names[] = {"error", "warning", "information", "hint"};
    return names[(int)severity];
}

const char *diagnostic_phase_name(DiagnosticPhase phase) {
    static const char *names[] = {
        "source", "syntax", "resolution", "typed", "effect", "qtt", "backend"
    };
    return names[(int)phase];
}

const char *diagnostic_applicability_name(DiagnosticApplicability applicability) {
    static const char *names[] = {
        "unspecified", "machine-applicable", "maybe-incorrect"
    };
    return names[(int)applicability];
}

const char *diagnostic_origin_name(DiagnosticOrigin origin) {
    static const char *names[] = {"compiler", "configuration", "external"};
    return names[(int)origin];
}
