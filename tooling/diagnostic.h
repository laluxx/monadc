#ifndef MONAD_TOOLING_DIAGNOSTIC_H
#define MONAD_TOOLING_DIAGNOSTIC_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    DIAGNOSTIC_ERROR,
    DIAGNOSTIC_WARNING,
    DIAGNOSTIC_INFORMATION,
    DIAGNOSTIC_HINT,
} DiagnosticSeverity;

typedef enum {
    DIAGNOSTIC_PHASE_SOURCE,
    DIAGNOSTIC_PHASE_SYNTAX,
    DIAGNOSTIC_PHASE_RESOLUTION,
    DIAGNOSTIC_PHASE_TYPED,
    DIAGNOSTIC_PHASE_EFFECT,
    DIAGNOSTIC_PHASE_QTT,
    DIAGNOSTIC_PHASE_BACKEND,
} DiagnosticPhase;

typedef enum {
    DIAGNOSTIC_EDIT_NONE,
    DIAGNOSTIC_EDIT_MACHINE_APPLICABLE,
    DIAGNOSTIC_EDIT_MAYBE_INCORRECT,
} DiagnosticApplicability;

typedef enum {
    DIAGNOSTIC_ORIGIN_COMPILER,
    DIAGNOSTIC_ORIGIN_CONFIGURATION,
    DIAGNOSTIC_ORIGIN_EXTERNAL,
} DiagnosticOrigin;

typedef struct {
    char *path;
    size_t line, column, end_line, end_column;
} DiagnosticSpan;

typedef struct {
    DiagnosticSpan span;
    char *replacement;
    DiagnosticApplicability applicability;
} DiagnosticEdit;

typedef struct {
    DiagnosticSpan span;
    char *message;
} DiagnosticRelated;

typedef struct {
    char *path;
    char *code;
    DiagnosticSeverity severity;
    DiagnosticPhase phase;
    DiagnosticApplicability applicability;
    DiagnosticOrigin origin;
    bool complete;
    size_t line, column, end_line, end_column;
    char *message;
    char *explanation;
    DiagnosticEdit *edits;
    size_t edit_count, edit_capacity;
    DiagnosticRelated *related;
    size_t related_count, related_capacity;
} Diagnostic;

typedef struct {
    Diagnostic *items;
    size_t count, capacity, files_checked;
    bool internal_error;
} DiagnosticSet;

void diagnostic_set_init(DiagnosticSet *set);
void diagnostic_dispose(Diagnostic *diagnostic);
void diagnostic_set_dispose(DiagnosticSet *set);
bool diagnostic_set_push(DiagnosticSet *set, Diagnostic *diagnostic);
bool diagnostic_add_edit(Diagnostic *diagnostic, const char *path,
                         size_t line, size_t column,
                         size_t end_line, size_t end_column,
                         const char *replacement,
                         DiagnosticApplicability applicability);
bool diagnostic_add_related(Diagnostic *diagnostic, const char *path,
                            size_t line, size_t column,
                            size_t end_line, size_t end_column,
                            const char *message);
const char *diagnostic_severity_name(DiagnosticSeverity severity);
const char *diagnostic_phase_name(DiagnosticPhase phase);
const char *diagnostic_applicability_name(DiagnosticApplicability applicability);
const char *diagnostic_origin_name(DiagnosticOrigin origin);

#endif
