#ifndef MONAD_SURFACE_RESULT_INTERNAL_H
#define MONAD_SURFACE_RESULT_INTERNAL_H

#include "include/monad/compiler.h"

struct monad_source_result {
    monad_compiler_diagnostic_set_t *diagnostics;
    monad_compiler_environment_t *environment;
    int published;
};

#endif
