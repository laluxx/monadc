#ifndef MONAD_TOOLING_LINT_H
#define MONAD_TOOLING_LINT_H

#include <stdbool.h>
#include <stddef.h>
#include "diagnostic.h"

typedef DiagnosticSeverity LintSeverity;
#define LINT_ERROR DIAGNOSTIC_ERROR
#define LINT_WARNING DIAGNOSTIC_WARNING
#define LINT_INFORMATION DIAGNOSTIC_INFORMATION
#define LINT_HINT DIAGNOSTIC_HINT

typedef DiagnosticPhase LintPhase;
#define LINT_PHASE_SOURCE DIAGNOSTIC_PHASE_SOURCE
#define LINT_PHASE_SYNTAX DIAGNOSTIC_PHASE_SYNTAX
#define LINT_PHASE_TYPED DIAGNOSTIC_PHASE_TYPED
#define LINT_PHASE_EFFECT DIAGNOSTIC_PHASE_EFFECT

typedef enum {
    LINT_FIX_NONE,
    LINT_FIX_SAFE,
    LINT_FIX_SUGGESTED,
} LintFixSafety;

typedef struct {
    const char *id;
    const char *title;
    LintPhase phase;
    LintSeverity default_severity;
    LintFixSafety fix_safety;
} LintRule;

typedef Diagnostic LintDiagnostic;
typedef DiagnosticSet LintResult;

void lint_result_init(LintResult *result);
void lint_result_dispose(LintResult *result);
const LintRule *lint_rule_find(const char *id);
bool lint_source(const char *path, const char *source, LintResult *result);
bool lint_path(const char *path, LintResult *result);
int cmd_lint(const char *path, bool json, bool fix);

#endif
