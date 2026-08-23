#ifndef MONAD_SURFACE_INTERNAL_H
#define MONAD_SURFACE_INTERNAL_H

#include "include/monad/monad.h"

typedef void (*monad_owned_resource_destroy_t)(void *resource);

typedef struct monad_surface_i64_export {
    const char *name;
    monad_i64_function_t address;
    const char *docstring;
} monad_surface_i64_export_t;

MONAD_API monad_status_t monad_surface_publish_i64_exports(
    monad_t *monad, const monad_surface_i64_export_t *exports, size_t count,
    void *resource, monad_owned_resource_destroy_t destroy,
    monad_error_t *error);

#endif
