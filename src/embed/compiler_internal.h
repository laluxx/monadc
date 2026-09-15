#ifndef MONAD_COMPILER_INTERNAL_H
#define MONAD_COMPILER_INTERNAL_H

#include "include/monad/compiler.h"
#include "frontend_transaction.h"
typedef bool (*monad_compiler_unit_hook_t)(
    const MonadCompilationUnit *unit, void **product,
    const char **failed_definition);

monad_status_t monad_compiler_analyze_source_hook(
    monad_compiler_session_t *session, const monad_compiler_source_t *source,
    monad_compiler_diagnostic_set_t **diagnostics,
    monad_compiler_environment_t **environment,
    monad_compiler_unit_hook_t hook, void **product, monad_error_t *error);

#endif
