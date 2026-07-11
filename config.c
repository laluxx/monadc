#include "config.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

/// Path helpers

static char *config_join_path(const char *left, const char *right)
{
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    size_t need_slash = left_len > 0 && left[left_len - 1] != '/';
    char *path = malloc(left_len + need_slash + right_len + 1);
    if (!path) return NULL;
    memcpy(path, left, left_len);
    if (need_slash) path[left_len++] = '/';
    memcpy(path + left_len, right, right_len);
    path[left_len + right_len] = '\0';
    return path;
}

static void config_make_dir(const char *path)
{
#if defined(_WIN32)
    int rc = _mkdir(path);
#else
    int rc = mkdir(path, 0755);
#endif
    if (rc != 0 && errno != EEXIST) {
        fprintf(stderr, "warning: cannot create config directory '%s': %s\n",
                path, strerror(errno));
    }
}

/// Flag parsing

static bool config_parse_optimization_flag(const char *arg, int *level)
{
    if (strcmp(arg, "-O") == 0 || strcmp(arg, "optimize") == 0 ||
        strcmp(arg, "--optimize") == 0) {
        *level = 1;
        return true;
    }
    if (arg[0] == '-' && arg[1] == 'O' &&
        arg[2] >= '0' && arg[2] <= '2' && arg[3] == '\0') {
        *level = arg[2] - '0';
        return true;
    }
    if (strncmp(arg, "optimize=", 9) == 0 &&
        arg[9] >= '0' && arg[9] <= '2' && arg[10] == '\0') {
        *level = arg[9] - '0';
        return true;
    }
    if (strncmp(arg, "--optimize=", 11) == 0 &&
        arg[11] >= '0' && arg[11] <= '2' && arg[12] == '\0') {
        *level = arg[11] - '0';
        return true;
    }
    return false;
}

static bool config_apply_trace_item(CompilerFlags *flags, const char *item)
{
    if (strcmp(item, "all") == 0) {
        flags->trace_ast = true;
        flags->trace_semantic = true;
        flags->trace_dep = true;
        flags->trace_codegen = true;
        return true;
    }
    if (strcmp(item, "ast") == 0 || strcmp(item, "reader") == 0) {
        flags->trace_ast = true;
        return true;
    }
    if (strcmp(item, "semantic") == 0 || strcmp(item, "opt") == 0) {
        flags->trace_semantic = true;
        return true;
    }
    if (strcmp(item, "dep") == 0 || strcmp(item, "type") == 0) {
        flags->trace_dep = true;
        return true;
    }
    if (strcmp(item, "codegen") == 0 || strcmp(item, "ir") == 0) {
        flags->trace_codegen = true;
        return true;
    }
    if (strcmp(item, "none") == 0 || strcmp(item, "off") == 0) {
        flags->trace_ast = false;
        flags->trace_semantic = false;
        flags->trace_dep = false;
        flags->trace_codegen = false;
        return true;
    }
    return false;
}

static void config_apply_trace_value(CompilerFlags *flags, const char *value)
{
    const char *p = value;
    char item[32];
    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != ',') n++;
        size_t copy_n = n < sizeof(item) - 1 ? n : sizeof(item) - 1;
        memcpy(item, p, copy_n);
        item[copy_n] = '\0';
        config_apply_trace_item(flags, item);
        p += n;
        if (*p == ',') p++;
    }
}

static bool config_apply_trace_flag(CompilerFlags *flags, const char *token)
{
    if (strcmp(token, "-v") == 0 || strcmp(token, "--verbose") == 0 ||
        strcmp(token, "verbose") == 0) {
        flags->verbose_level++;
        return true;
    }
    if (strcmp(token, "-vv") == 0) {
        flags->verbose_level += 2;
        return true;
    }
    if (strcmp(token, "--quiet") == 0 || strcmp(token, "quiet") == 0) {
        flags->verbose_level = 0;
        flags->trace_ast = false;
        flags->trace_semantic = false;
        flags->trace_dep = false;
        flags->trace_codegen = false;
        return true;
    }
    if (strcmp(token, "verbose") == 0) {
        flags->verbose_level++;
        return true;
    }
    if (strncmp(token, "--trace=", 8) == 0) {
        config_apply_trace_value(flags, token + 8);
        return true;
    }
    return false;
}

static void config_apply_flag_token(CompilerFlags *flags, const char *token)
{
    if (!token || !*token) return;
    if (strcmp(token, "--emit-ir") == 0 || strcmp(token, "emit-ir") == 0 ||
        strcmp(token, "--emit-llvm") == 0 || strcmp(token, "emit-llvm") == 0) {
        flags->emit_ir = true;
        return;
    }
    if (strcmp(token, "--emit-bc") == 0 || strcmp(token, "emit-bc") == 0 ||
        strcmp(token, "--bitcode") == 0 || strcmp(token, "bitcode") == 0) {
        flags->emit_bc = true;
        return;
    }
    if (strcmp(token, "--emit-asm") == 0 || strcmp(token, "emit-asm") == 0 ||
        strcmp(token, "-S") == 0 || strcmp(token, "asm") == 0) {
        flags->emit_asm = true;
        return;
    }
    if (strcmp(token, "--emit-obj") == 0 || strcmp(token, "emit-obj") == 0 ||
        strcmp(token, "-c") == 0 || strcmp(token, "obj") == 0) {
        flags->emit_obj = true;
        return;
    }
    if (strcmp(token, "--emit-json") == 0 || strcmp(token, "emit-json") == 0) {
        flags->emit_json = true;
        return;
    }
    if (strcmp(token, "--emit-typst") == 0 || strcmp(token, "emit-typst") == 0) {
        flags->emit_typst = true;
        return;
    }
    if (strcmp(token, "--emit-bytecode") == 0 || strcmp(token, "emit-bytecode") == 0 ||
        strcmp(token, "--bytecode") == 0 || strcmp(token, "bytecode") == 0) {
        flags->emit_bytecode = true;
        return;
    }
    if (strcmp(token, "--bytecode-verify") == 0 || strcmp(token, "bytecode-verify") == 0) {
        flags->bytecode_verify = true;
        return;
    }
    if (strcmp(token, "--bytecode-disassemble") == 0 || strcmp(token, "bytecode-disassemble") == 0 ||
        strcmp(token, "--bytecode-dump") == 0 || strcmp(token, "bytecode-dump") == 0) {
        flags->bytecode_disassemble = true;
        return;
    }
    if (strcmp(token, "--bytecode-decompile") == 0 || strcmp(token, "bytecode-decompile") == 0) {
        flags->bytecode_decompile = true;
        return;
    }
    if (strcmp(token, "--bytecode-sections") == 0 || strcmp(token, "bytecode-sections") == 0) {
        flags->bytecode_dump_sections = true;
        return;
    }
    if (strcmp(token, "--bytecode-trace") == 0 || strcmp(token, "bytecode-trace") == 0) {
        flags->bytecode_trace = true;
        return;
    }
    if (strcmp(token, "--bytecode-baseline-jit") == 0 || strcmp(token, "bytecode-baseline-jit") == 0) {
        flags->bytecode_baseline_jit = true;
        return;
    }
    if (strcmp(token, "-jit") == 0 || strcmp(token, "jit") == 0 || strcmp(token, "--jit") == 0) {
        flags->jit = true;
        flags->emit_bytecode = true;
        flags->bytecode_baseline_jit = true;
        return;
    }
    if (strcmp(token, "--test") == 0 || strcmp(token, "test") == 0) {
        flags->test_mode = true;
        return;
    }
    if (strcmp(token, "-i") == 0 || strcmp(token, "repl") == 0 ||
        strcmp(token, "--repl") == 0 || strcmp(token, "--interactive") == 0 ||
        strcmp(token, "interactive") == 0) {
        flags->start_repl = true;
        return;
    }
    if (config_parse_optimization_flag(token, &flags->optimization_level)) return;
    if (config_apply_trace_flag(flags, token)) return;
}

/// Public API

void config_apply_default_flags(CompilerFlags *flags)
{
    const char *home = getenv("HOME");
    if (!flags || !home || !*home) return;

    char *config_dir = config_join_path(home, ".config");
    char *monad_dir = config_join_path(config_dir ? config_dir : home, "monad");
    char *flags_path = config_join_path(monad_dir ? monad_dir : home, "flags");
    if (!config_dir || !monad_dir || !flags_path) goto done;

    config_make_dir(config_dir);
    config_make_dir(monad_dir);

    FILE *f = fopen(flags_path, "r");
    if (!f) {
        f = fopen(flags_path, "w");
        if (f) fclose(f);
        goto done;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *save = NULL;
        char *tok = strtok_r(line, " \t\r\n", &save);
        while (tok) {
            if (strcmp(tok, "trace") == 0) {
                char *value = strtok_r(NULL, " \t\r\n", &save);
                if (value) config_apply_trace_value(flags, value);
            } else {
                config_apply_flag_token(flags, tok);
            }
            tok = strtok_r(NULL, " \t\r\n", &save);
        }
    }
    fclose(f);

done:
    free(config_dir);
    free(monad_dir);
    free(flags_path);
}
