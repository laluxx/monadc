#include "include/monad/monad.h"
#include "include/monad/compiler.h"
#include "include/monad/qtt.h"
#include "surface_result_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static monad_status_t source_surface_fail(
    monad_error_t *error, monad_status_t status, const char *message) {
    if (error) {
        error->status = status;
        error->message = message;
    }
    return status;
}

monad_status_t monad_prepare_string(
    monad_t *monad, const char *source_name, const char *source,
    monad_source_result_t **output, monad_error_t *error) {
    if (!source)
        return source_surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                                   "source is required");
    return monad_prepare_source(
        monad, source_name, source, strlen(source), output, error);
}

monad_status_t monad_prepare_source(
    monad_t *monad, const char *source_name, const char *source,
    size_t byte_count, monad_source_result_t **output, monad_error_t *error) {
    if (!output)
        return source_surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                                   "source result output is required");
    *output = NULL;
    if (!monad || !source_name || (!source && byte_count))
        return source_surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                                   "Monad host, source name, and source are required");

    monad_compiler_source_descriptor_t descriptor;
    monad_compiler_source_t *unit = NULL;
    monad_compiler_session_t *session = NULL;
    monad_source_result_t *result = calloc(1, sizeof(*result));
    if (!result)
        return source_surface_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                                   "could not allocate source result");

    monad_compiler_source_descriptor_init(&descriptor);
    descriptor.name = source_name;
    descriptor.bytes = source;
    descriptor.byte_count = byte_count;
    descriptor.version = 1;
    monad_status_t status = monad_compiler_source_create(
        &descriptor, &unit, error);
    if (status == MONAD_OK)
        status = monad_compiler_session_create(&session, error);
    if (status == MONAD_OK)
        status = monad_compiler_analyze_source(
            session, unit, &result->diagnostics, &result->environment, error);
    monad_compiler_session_destroy(session);
    monad_compiler_source_destroy(unit);
    if (status != MONAD_OK) {
        monad_source_result_destroy(result);
        return status;
    }
    *output = result;
    return MONAD_OK;
}

monad_status_t monad_prepare_file(
    monad_t *monad, const char *path,
    monad_source_result_t **output, monad_error_t *error) {
    if (!output)
        return source_surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                                   "source result output is required");
    *output = NULL;
    if (!monad || !path || !path[0])
        return source_surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                                   "Monad host and source path are required");
    FILE *file = fopen(path, "rb");
    if (!file)
        return source_surface_fail(error, MONAD_ERROR_IO,
                                   "could not open Monad source file");
    size_t count = 0;
    size_t capacity = 4096;
    char *bytes = malloc(capacity);
    if (!bytes) {
        fclose(file);
        return source_surface_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                                   "could not allocate source file buffer");
    }
    while (!feof(file)) {
        if (count == capacity) {
            if (capacity > SIZE_MAX / 2) {
                free(bytes);
                fclose(file);
                return source_surface_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                                           "source file is too large");
            }
            capacity *= 2;
            char *grown = realloc(bytes, capacity);
            if (!grown) {
                free(bytes);
                fclose(file);
                return source_surface_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                                           "could not grow source file buffer");
            }
            bytes = grown;
        }
        size_t read_count = fread(bytes + count, 1, capacity - count, file);
        count += read_count;
        if (ferror(file)) {
            free(bytes);
            fclose(file);
            return source_surface_fail(error, MONAD_ERROR_IO,
                                       "could not read Monad source file");
        }
    }
    if (fclose(file) != 0) {
        free(bytes);
        return source_surface_fail(error, MONAD_ERROR_IO,
                                   "could not close Monad source file");
    }
    monad_status_t status = monad_prepare_source(
        monad, path, bytes, count, output, error);
    free(bytes);
    return status;
}

void monad_source_result_destroy(monad_source_result_t *result) {
    if (!result) return;
    monad_compiler_environment_destroy(result->environment);
    monad_compiler_diagnostic_set_destroy(result->diagnostics);
    free(result);
}

int monad_source_result_is_valid(const monad_source_result_t *result) {
    return result && result->environment &&
        monad_compiler_diagnostic_count(result->diagnostics) == 0;
}

int monad_source_result_is_published(const monad_source_result_t *result) {
    return result && result->published;
}

size_t monad_source_result_diagnostic_count(
    const monad_source_result_t *result) {
    return result
        ? monad_compiler_diagnostic_count(result->diagnostics) : 0;
}

const char *monad_source_result_diagnostic_code(
    const monad_source_result_t *result, size_t index) {
    const monad_compiler_diagnostic_t *diagnostic = result
        ? monad_compiler_diagnostic_at(result->diagnostics, index) : NULL;
    return diagnostic ? diagnostic->code : NULL;
}

const char *monad_source_result_diagnostic_message(
    const monad_source_result_t *result, size_t index) {
    const monad_compiler_diagnostic_t *diagnostic = result
        ? monad_compiler_diagnostic_at(result->diagnostics, index) : NULL;
    return diagnostic ? diagnostic->message : NULL;
}

size_t monad_source_result_definition_count(
    const monad_source_result_t *result) {
    return result
        ? monad_compiler_environment_count(result->environment) : 0;
}

const monad_qtt_report_t *monad_source_result_qtt_report(
    const monad_source_result_t *result) {
    return result
        ? monad_compiler_environment_qtt_report(result->environment) : NULL;
}
