#include "spirv.h"
#include "compat.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static char *format_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) {
        va_end(args);
        return strdup("SPIR-V frontend failed");
    }
    char *message = malloc((size_t)length + 1);
    if (message) vsnprintf(message, (size_t)length + 1, format, args);
    va_end(args);
    return message;
}

static int run_tool(char *const arguments[]) {
#ifdef _WIN32
    return (int)_spawnvp(_P_WAIT, arguments[0],
                         (const char *const *)arguments);
#else
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execvp(arguments[0], arguments);
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static int make_temporary_path(char *path, size_t capacity) {
#ifdef _WIN32
    const char *base = getenv("TEMP");
    if (!base) base = ".";
    if (snprintf(path, capacity, "%s\\monad-spirv-XXXXXX", base) >=
        (int)capacity) return -1;
    if (_mktemp_s(path, capacity) != 0) return -1;
    FILE *created = fopen(path, "wb");
    if (!created) return -1;
    fclose(created);
    return 0;
#else
    if (snprintf(path, capacity, "/tmp/monad-spirv-XXXXXX") >=
        (int)capacity) return -1;
    int descriptor = mkstemp(path);
    if (descriptor < 0) return -1;
    close(descriptor);
    return 0;
#endif
}

static int read_words(const char *path,
                      uint32_t **words_out,
                      size_t *count_out,
                      char **error_out) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        *error_out = format_error("cannot read generated SPIR-V '%s': %s",
                                  path, strerror(errno));
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        *error_out = format_error("cannot measure generated SPIR-V '%s'", path);
        return 0;
    }
    long bytes = ftell(file);
    rewind(file);
    if (bytes < 20 || (bytes % 4) != 0) {
        fclose(file);
        *error_out = format_error("'%s' is not a complete SPIR-V module", path);
        return 0;
    }
    size_t count = (size_t)bytes / 4;
    uint32_t *words = malloc((size_t)bytes);
    if (!words || fread(words, 4, count, file) != count) {
        free(words);
        fclose(file);
        *error_out = format_error("cannot load generated SPIR-V '%s'", path);
        return 0;
    }
    fclose(file);
    if (words[0] != UINT32_C(0x07230203)) {
        free(words);
        *error_out = format_error("'%s' has an invalid SPIR-V magic word", path);
        return 0;
    }
    *words_out = words;
    *count_out = count;
    return 1;
}

static int compile_shader(const char *source_path,
                          uint32_t **words_out,
                          size_t *count_out,
                          char **error_out) {
    struct stat information;
    if (stat(source_path, &information) != 0) {
        *error_out = format_error("SPIR-V source '%s' does not exist", source_path);
        return 0;
    }

    char temporary[1024];
    if (make_temporary_path(temporary, sizeof(temporary)) != 0) {
        *error_out = format_error("cannot create a temporary SPIR-V output: %s",
                                  strerror(errno));
        return 0;
    }

    char *compile_arguments[] = {
        "glslc", "--target-env=vulkan1.3", (char *)source_path,
        "-o", temporary, NULL
    };
    int compiled = run_tool(compile_arguments);
    if (compiled != 0) {
        remove(temporary);
        *error_out = format_error("glslc rejected '%s' (status %d)",
                                  source_path, compiled);
        return 0;
    }

    char *validate_arguments[] = {
        "spirv-val", "--target-env", "vulkan1.3", temporary, NULL
    };
    int validated = run_tool(validate_arguments);
    if (validated != 0) {
        remove(temporary);
        *error_out = format_error("spirv-val rejected '%s' (status %d)",
                                  source_path, validated);
        return 0;
    }

    int loaded = read_words(temporary, words_out, count_out, error_out);
    remove(temporary);
    return loaded;
}

static int identifier_character(char character) {
    return isalnum((unsigned char)character) || character == '_' ||
           character == '-' || character == '?' || character == '!';
}

static const char *path_basename(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
#ifdef _WIN32
    const char *backslash = path ? strrchr(path, '\\') : NULL;
    if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
    return slash ? slash + 1 : path;
}

static char *module_name_from_output(const char *path) {
    const char *base = path_basename(path);
    if (!base) return NULL;
    size_t length = strlen(base);
    if (length <= 4 || strcmp(base + length - 4, ".mon") != 0)
        return NULL;
    char *name = strndup(base, length - 4);
    if (!name || !name[0] || !isupper((unsigned char)name[0])) {
        free(name);
        return NULL;
    }
    for (char *cursor = name; *cursor; cursor++) {
        if (!isalnum((unsigned char)*cursor) && *cursor != '_') {
            free(name);
            return NULL;
        }
    }
    return name;
}

static char *binding_name_from_source(const char *path) {
    const char *base = path_basename(path);
    if (!base || !base[0]) return NULL;
    const char *dot = strrchr(base, '.');
    size_t length = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    char *name = strndup(base, length);
    if (!name || !name[0]) {
        free(name);
        return NULL;
    }
    for (char *cursor = name; *cursor; cursor++)
        if (!identifier_character(*cursor)) *cursor = '-';
    return name;
}

static char *output_name_from_source(const char *path) {
    const char *base = path_basename(path);
    if (!base || !base[0]) return NULL;
    /* Keep the shader stage in the derived module name so fragment and
     * vertex sources never collide: Triangle.frag -> TriangleFrag.mon and
     * Triangle.vert -> TriangleVert.mon. */
    const char *dot = strrchr(base, '.');
    int has_extension = dot && dot != base;
    size_t stem_length = has_extension ? (size_t)(dot - base) : strlen(base);
    const char *extension = has_extension ? dot + 1 : NULL;
    size_t extension_length = has_extension ? strlen(extension) : 0;
    if (stem_length == 0) return NULL;
    size_t length = stem_length + extension_length;
    char *name = malloc(length + 5);
    if (!name) return NULL;
    memcpy(name, base, stem_length);
    if (has_extension) {
        memcpy(name + stem_length, extension, extension_length);
        name[stem_length] = (char)toupper((unsigned char)name[stem_length]);
    }
    name[0] = (char)toupper((unsigned char)name[0]);
    for (size_t index = 0; index < length; index++)
        if (name[index] == '-') name[index] = '_';
    memcpy(name + length, ".mon", 5);
    return name;
}

int spirv_write_monad_module(const char *source_path,
                             const char *output_path,
                             const char *binding_name,
                             char **error_out) {
    if (error_out) *error_out = NULL;
    if (!source_path || !source_path[0]) {
        if (error_out) *error_out = format_error("missing shader source");
        return 0;
    }
    char *derived_output = output_path && output_path[0]
        ? NULL : output_name_from_source(source_path);
    const char *destination = output_path && output_path[0]
        ? output_path : derived_output;
    char *module_name = module_name_from_output(destination);
    if (!module_name) {
        if (error_out) *error_out = format_error(
            "output must be a valid capitalized Monad module ending in .mon");
        free(derived_output);
        return 0;
    }
    char *derived_name = binding_name && binding_name[0]
        ? NULL : binding_name_from_source(source_path);
    const char *name = binding_name && binding_name[0]
        ? binding_name : derived_name;
    if (!name) {
        if (error_out) *error_out = format_error(
            "cannot derive a binding name from '%s'", source_path);
        free(module_name);
        free(derived_output);
        return 0;
    }
    for (const char *cursor = name; *cursor; cursor++) {
        if (!identifier_character(*cursor)) {
            if (error_out) *error_out = format_error(
                "'%s' is not a valid Monad binding name", name);
            free(module_name);
            free(derived_name);
            free(derived_output);
            return 0;
        }
    }

    uint32_t *words = NULL;
    size_t count = 0;
    char *compile_error = NULL;
    if (!compile_shader(source_path, &words, &count, &compile_error)) {
        if (error_out) *error_out = compile_error;
        else free(compile_error);
        free(module_name);
        free(derived_name);
        free(derived_output);
        return 0;
    }

    FILE *output = fopen(destination, "wb");
    if (!output) {
        if (error_out) *error_out = format_error(
            "cannot write '%s': %s", destination, strerror(errno));
        free(words);
        free(module_name);
        free(derived_name);
        free(derived_output);
        return 0;
    }

    fprintf(output, ";;; %s --- SPIR-V embedded from %s\n\n",
            path_basename(destination), path_basename(source_path));
    fprintf(output, "module %s\n\n", module_name);
    fprintf(output, "define %s-size :: Int %zu\n\n",
            name, count * sizeof(uint32_t));
    fprintf(output, "define %s :: [%zu U32]\n[\n", name, count);
    for (size_t index = 0; index < count; index++) {
        if (index % 6 == 0) fputs("  ", output);
        else fputc(' ', output);
        fprintf(output, "0x%08x", words[index]);
        if (index % 6 == 5 || index + 1 == count) fputc('\n', output);
    }
    fputs("]\n", output);

    int okay = fclose(output) == 0;
    if (!okay && error_out)
        *error_out = format_error("cannot finish writing '%s'", destination);
    free(words);
    free(module_name);
    free(derived_name);
    free(derived_output);
    return okay;
}
