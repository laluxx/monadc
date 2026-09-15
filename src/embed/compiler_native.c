#include "compiler_internal.h"
#include "native_compile.h"

static bool build_native(
    const MonadCompilationUnit *unit, void **product,
    const char **failed_definition) {
    return monad_native_image_build(
        unit, (MonadNativeImage **)product, failed_definition);
}

monad_status_t monad_compiler_analyze_native_source(
    monad_compiler_session_t *session, const monad_compiler_source_t *source,
    monad_compiler_diagnostic_set_t **diagnostics,
    monad_compiler_environment_t **environment, MonadNativeImage **image,
    monad_error_t *error) {
    return monad_compiler_analyze_source_hook(
        session, source, diagnostics, environment, build_native,
        (void **)image, error);
}
