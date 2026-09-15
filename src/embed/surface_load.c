#include "include/monad/monad.h"
#include "compiler_internal.h"
#include "native_compile.h"
#include "surface_internal.h"
#include "surface_result_internal.h"

#include <stdlib.h>
#include <string.h>

monad_status_t monad_compiler_analyze_native_source(
    monad_compiler_session_t *, const monad_compiler_source_t *,
    monad_compiler_diagnostic_set_t **, monad_compiler_environment_t **,
    MonadNativeImage **, monad_error_t *);

static monad_status_t load_fail(
    monad_error_t *error, monad_status_t status, const char *message) {
    if (error) { error->status = status; error->message = message; }
    return status;
}

monad_status_t monad_load_string(
    monad_t *monad, const char *source_name, const char *source,
    monad_source_result_t **output, monad_error_t *error) {
    if (!monad || !source_name || !output || !source)
        return load_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                         "Monad host, source name, source, and result are required");
    *output = NULL;
    monad_compiler_source_descriptor_t descriptor;
    monad_compiler_source_t *unit = NULL;
    monad_compiler_session_t *session = NULL;
    MonadNativeImage *image = NULL;
    monad_source_result_t *result = calloc(1, sizeof(*result));
    if (!result)
        return load_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                         "could not allocate source result");
    monad_compiler_source_descriptor_init(&descriptor);
    descriptor.name = source_name;
    descriptor.bytes = source;
    descriptor.byte_count = strlen(source);
    descriptor.version = 1;
    monad_status_t status = monad_compiler_source_create(
        &descriptor, &unit, error);
    if (status == MONAD_OK)
        status = monad_compiler_session_create(&session, error);
    if (status == MONAD_OK)
        status = monad_compiler_analyze_native_source(
            session, unit, &result->diagnostics, &result->environment,
            &image, error);
    monad_compiler_session_destroy(session);
    monad_compiler_source_destroy(unit);
    if (status != MONAD_OK) {
        monad_native_image_destroy(image);
        monad_source_result_destroy(result);
        return status;
    }
    if (monad_source_result_is_valid(result)) {
        size_t export_count = monad_native_image_count(image);
        monad_surface_i64_export_t *exports =
            calloc(export_count, sizeof(*exports));
        if (!exports) {
            monad_native_image_destroy(image);
            monad_source_result_destroy(result);
            return load_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                             "could not stage native exports");
        }
        for (size_t i = 0; i < export_count; i++)
            exports[i] = (monad_surface_i64_export_t){
                monad_native_image_name(image, i),
                (monad_i64_function_t)monad_native_image_address(image, i),
                monad_native_image_docstring(image, i)};
        status = monad_surface_publish_i64_exports(
            monad, exports, export_count, image,
            monad_native_image_destroy, error);
        image = NULL;
        free(exports);
        if (status != MONAD_OK) {
            monad_source_result_destroy(result);
            return status;
        }
        result->published = 1;
    }
    *output = result;
    return MONAD_OK;
}
