#ifndef MONAD_MONAD_H
#define MONAD_MONAD_H

#include "embed.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The beginner-facing embedding API. monad_t owns its runtime, current-thread
 * attachment, and every binding acquired through this facade. Advanced users
 * can opt into embed.h and compiler.h without paying for hidden dispatch here.
 *
 * This one-owner entry boundary is informed by Guile's scm_with_guile:
 * https://www.gnu.org/software/guile/manual/html_node/Guile-Initialization-Functions.html
 */
typedef struct monad monad_t;
typedef struct monad_source_result monad_source_result_t;
typedef struct monad_read monad_read_t;
typedef struct monad_i64_handle monad_i64_handle_t;
typedef int64_t (*monad_i64_function_t)(int64_t value);

MONAD_API monad_status_t monad_open(monad_t **monad, monad_error_t *error);
MONAD_API monad_status_t monad_close(monad_t *monad, monad_error_t *error);

/* Register a host function under a Monad name. The facade retains the native
 * binding until monad_close; the hot call remains an ordinary C ABI call. */
MONAD_API monad_status_t monad_define_i64_function(
    monad_t *monad, const char *name, monad_i64_function_t function,
    const char *docstring, monad_error_t *error);

/* Resolve and validate Int -> Int once. The returned pointer has no boxing,
 * allocation, or embedding dispatch and remains valid until monad_close. */
MONAD_API monad_status_t monad_find_i64_function(
    monad_t *monad, const char *name, monad_i64_function_t *function,
    monad_error_t *error);

/* Explicit frozen lifetime. The extracted pointer is valid until release and
 * remains an ordinary C ABI call with no handle dispatch on its hot path. The
 * explicit ownership boundary follows ORCv2 removable-resource handles:
 * https://llvm.org/docs/ORCv2.html#how-to-remove-code */
MONAD_API monad_status_t monad_find_i64_handle(
    monad_t *monad, const char *name, monad_i64_handle_t **handle,
    monad_error_t *error);
MONAD_API monad_i64_function_t monad_i64_handle_address(
    const monad_i64_handle_t *handle);
MONAD_API void monad_i64_handle_release(monad_i64_handle_t *handle);

/* Optional coherent phase boundary. Begin snapshots every compatible native
 * address at one registry epoch; lookups remain direct function pointers and
 * add no per-call wrapper. End releases the pinned generation. */
MONAD_API monad_status_t monad_read_begin(
    monad_t *monad, monad_read_t **read, monad_error_t *error);
MONAD_API uint64_t monad_read_generation(const monad_read_t *read);
MONAD_API monad_status_t monad_read_find_i64_function(
    const monad_read_t *read, const char *name,
    monad_i64_function_t *function, monad_error_t *error);
MONAD_API void monad_read_end(monad_read_t *read);
/* Advanced lifecycle observation; useful for hosts validating reload policy. */
MONAD_API size_t monad_live_native_image_count(const monad_t *monad);

/* Prepare source through parsing and HM inference. This operation does not
 * claim native publication: a valid result is semantic input for the future
 * QTT/codegen/commit stages. The result owns all diagnostics and definition
 * metadata and is independent of the caller's source buffer. */
MONAD_API monad_status_t monad_prepare_string(
    monad_t *monad, const char *source_name, const char *source,
    monad_source_result_t **result, monad_error_t *error);
/* Compile, verify, and atomically publish definitions from an owned string.
 * Successful Int -> Int lookup is still a direct C ABI function pointer. */
MONAD_API monad_status_t monad_load_string(
    monad_t *monad, const char *source_name, const char *source,
    monad_source_result_t **result, monad_error_t *error);
/* These are the owned input counterparts of Guile's eval/load hooks. Monad
 * preparation compiles semantic evidence but does not claim execution:
 * https://www.gnu.org/software/guile/manual/html_node/Dia-Hook.html
 * https://www.gnu.org/software/guile/manual/html_node/Loading.html
 */
MONAD_API monad_status_t monad_prepare_source(
    monad_t *monad, const char *source_name, const char *source,
    size_t byte_count, monad_source_result_t **result, monad_error_t *error);
MONAD_API monad_status_t monad_prepare_file(
    monad_t *monad, const char *path,
    monad_source_result_t **result, monad_error_t *error);
MONAD_API void monad_source_result_destroy(monad_source_result_t *result);
MONAD_API int monad_source_result_is_valid(
    const monad_source_result_t *result);
MONAD_API int monad_source_result_is_published(
    const monad_source_result_t *result);
MONAD_API size_t monad_source_result_diagnostic_count(
    const monad_source_result_t *result);
MONAD_API const char *monad_source_result_diagnostic_code(
    const monad_source_result_t *result, size_t index);
MONAD_API const char *monad_source_result_diagnostic_message(
    const monad_source_result_t *result, size_t index);
MONAD_API size_t monad_source_result_definition_count(
    const monad_source_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
